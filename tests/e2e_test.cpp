// =============================================================================
// tests/e2e_test.cpp — Automated unit & end-to-end integration test suite
//
// Tests:
//   1. SHA1 RFC 3174 test vectors
//   2. Bencode encode/decode round-trip
//   3. Metainfo parser (single-file and multi-file + info_hash verification)
//   4. PieceManager lazy RAM allocation, pipeline deduplication, and hash fail recovery
//   5. Full end-to-end Session download over real TCP sockets (HTTP tracker + BEP 3 peer)
// =============================================================================

#include "bencode.hpp"
#include "metainfo.hpp"
#include "peer_wire.hpp"
#include "piece_manager.hpp"
#include "session.hpp"
#include "sha1.hpp"
#include "tracker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            std::exit(1); \
        } \
    } while (0)

// =============================================================================
// 1. SHA1 Test
// =============================================================================

void test_sha1() {
    std::printf("[TEST 1/5] SHA1 RFC 3174 vectors...\n");

    auto d1 = bt::SHA1::hash("abc", 3);
    TEST_ASSERT(bt::SHA1::to_hex(d1) == "a9993e364706816aba3e25717850c26c9cd0d89d",
                "SHA1('abc') mismatch");

    const char* v2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auto d2 = bt::SHA1::hash(v2, std::strlen(v2));
    TEST_ASSERT(bt::SHA1::to_hex(d2) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
                "SHA1(448-bit vector) mismatch");

    std::printf("  -> PASS\n");
}

// =============================================================================
// 2. Bencode Test
// =============================================================================

void test_bencode() {
    std::printf("[TEST 2/5] Bencode encode/decode round-trip...\n");

    bt::BDict root;
    root["int_val"] = bt::BValue(int64_t(-42));
    root["str_val"] = bt::BValue(std::string("ps4-torrent"));
    bt::BList list;
    list.emplace_back(int64_t(1));
    list.emplace_back(std::string("two"));
    root["list_val"] = bt::BValue(list);

    std::string encoded = bt::encode(bt::BValue(root));
    bt::BValue decoded  = bt::decode(encoded);

    TEST_ASSERT(decoded.is_dict(), "Root must be dict");
    TEST_ASSERT(decoded.as_dict().at("int_val").as_int() == -42, "Int mismatch");
    TEST_ASSERT(decoded.as_dict().at("str_val").as_string() == "ps4-torrent", "String mismatch");
    TEST_ASSERT(decoded.as_dict().at("list_val").as_list().size() == 2, "List size mismatch");

    std::printf("  -> PASS\n");
}

// =============================================================================
// 3. PieceManager Pipeline & Lazy Allocation Test
// =============================================================================

bt::Metainfo make_synthetic_torrent(
    const std::string& announce_url,
    const std::vector<uint8_t>& payload,
    int64_t piece_len,
    std::string& out_raw_torrent)
{
    size_t num_pieces = (payload.size() + piece_len - 1) / piece_len;
    std::string pieces_concat;
    pieces_concat.reserve(num_pieces * 20);

    for (size_t i = 0; i < num_pieces; ++i) {
        size_t offset = i * static_cast<size_t>(piece_len);
        size_t len    = std::min(static_cast<size_t>(piece_len), payload.size() - offset);
        auto digest   = bt::SHA1::hash(payload.data() + offset, len);
        pieces_concat.append(reinterpret_cast<const char*>(digest.data()), 20);
    }

    // Multi-file layout: split payload across 2 files
    int64_t file1_len = static_cast<int64_t>(payload.size() / 3);
    int64_t file2_len = static_cast<int64_t>(payload.size()) - file1_len;

    bt::BDict f1;
    f1["length"] = bt::BValue(file1_len);
    f1["path"]   = bt::BValue(bt::BList{bt::BValue(std::string("sub")), bt::BValue(std::string("part1.bin"))});

    bt::BDict f2;
    f2["length"] = bt::BValue(file2_len);
    f2["path"]   = bt::BValue(bt::BList{bt::BValue(std::string("part2.bin"))});

    bt::BDict info;
    info["name"]         = bt::BValue(std::string("test_bundle"));
    info["piece length"] = bt::BValue(piece_len);
    info["pieces"]       = bt::BValue(pieces_concat);
    info["files"]        = bt::BValue(bt::BList{bt::BValue(f1), bt::BValue(f2)});

    bt::BDict root;
    root["announce"] = bt::BValue(announce_url);
    root["info"]     = bt::BValue(info);

    out_raw_torrent = bt::encode(bt::BValue(root));
    return bt::Metainfo::parse(out_raw_torrent);
}

void test_piece_manager() {
    std::printf("[TEST 3/5] PieceManager pipeline deduplication & hash verification...\n");

    // 64 KiB payload = 2 pieces of 32 KiB (each piece has 2 blocks of 16 KiB)
    std::vector<uint8_t> payload(64 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 131 + 17) & 0xFF);
    }

    std::string raw_torrent;
    bt::Metainfo meta = make_synthetic_torrent(
        "http://127.0.0.1:8080/announce", payload, 32 * 1024, raw_torrent);

    std::string tmp_dir = "/tmp/ps4_torrent_pm_test_" + std::to_string(::getpid());
    bt::PieceManager pm(meta, tmp_dir);

    // Peer has all 2 pieces (bitfield = 0xC0)
    std::vector<uint8_t> peer_bf = {0xC0};

    // Request 4 blocks in pipeline without receiving any yet:
    // Each call MUST return a distinct (piece_index, begin) pair!
    auto r1 = pm.next_request(peer_bf);
    auto r2 = pm.next_request(peer_bf);
    auto r3 = pm.next_request(peer_bf);
    auto r4 = pm.next_request(peer_bf);

    TEST_ASSERT(r1.has_value() && r2.has_value() && r3.has_value() && r4.has_value(),
                "Expected 4 valid block requests");
    TEST_ASSERT(r1->piece_index == 0 && r1->begin == 0,     "r1 should be piece 0 block 0");
    TEST_ASSERT(r2->piece_index == 0 && r2->begin == 16384, "r2 should be piece 0 block 1");
    TEST_ASSERT(r3->piece_index == 1 && r3->begin == 0,     "r3 should be piece 1 block 0");
    TEST_ASSERT(r4->piece_index == 1 && r4->begin == 16384, "r4 should be piece 1 block 1");

    // Test release_requests when a peer chokes/disconnects
    pm.release_requests({*r2});

    // Feed corrupt data for piece 0 to verify SHA1 rejection & recovery
    std::vector<uint8_t> bad_block(16384, 0xFF);
    bool ok0 = pm.receive_block(0, 0, bad_block);
    TEST_ASSERT(!ok0, "First block should not complete piece 0");
    bool ok1 = pm.receive_block(0, 16384, bad_block);
    TEST_ASSERT(!ok1, "Corrupt piece 0 must fail SHA1 verification");
    TEST_ASSERT(pm.pieces_done() == 0, "Corrupt piece must not increment pieces_done");

    // Now feed the real blocks for piece 0 and piece 1
    std::vector<uint8_t> p0_b0(payload.begin(), payload.begin() + 16384);
    std::vector<uint8_t> p0_b1(payload.begin() + 16384, payload.begin() + 32768);
    std::vector<uint8_t> p1_b0(payload.begin() + 32768, payload.begin() + 49152);
    std::vector<uint8_t> p1_b1(payload.begin() + 49152, payload.end());

    TEST_ASSERT(!pm.receive_block(0, 0, p0_b0), "p0_b0 incomplete");
    TEST_ASSERT(pm.receive_block(0, 16384, p0_b1), "p0_b1 should complete & verify piece 0");
    TEST_ASSERT(!pm.receive_block(1, 0, p1_b0), "p1_b0 incomplete");
    TEST_ASSERT(pm.receive_block(1, 16384, p1_b1), "p1_b1 should complete & verify piece 1");

    TEST_ASSERT(pm.is_complete(), "PieceManager should be 100% complete");
    TEST_ASSERT(pm.bytes_downloaded() == static_cast<int64_t>(payload.size()),
                "Exact bytes_downloaded mismatch");

    std::string cmd = "rm -rf " + tmp_dir;
    std::system(cmd.c_str());
    std::printf("  -> PASS\n");
}

// =============================================================================
// 4 & 5. Full End-to-End Session Test (Local HTTP Tracker + TCP Peer Seeder)
// =============================================================================

int create_listen_socket(uint16_t& out_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(fd >= 0, "socket() failed");

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; // Ephemeral port

    TEST_ASSERT(::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0,
                "bind() failed");
    TEST_ASSERT(::listen(fd, 8) == 0, "listen() failed");

    socklen_t len = sizeof(addr);
    TEST_ASSERT(::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0,
                "getsockname() failed");
    out_port = ntohs(addr.sin_port);
    return fd;
}

void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >>  8) & 0xFF;
    p[3] = (v >>  0) & 0xFF;
}

uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
           (static_cast<uint32_t>(p[3]) <<  0);
}

bool send_all(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* data, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (len > 0) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void test_end_to_end_session() {
    std::printf("[TEST 4/5] Starting local HTTP Tracker & BEP 3 Seeder Peer...\n");

    // 256 KiB payload across 8 pieces of 32 KiB
    constexpr size_t PAYLOAD_SIZE = 256 * 1024;
    constexpr int64_t PIECE_LEN   = 32 * 1024;

    std::vector<uint8_t> payload(PAYLOAD_SIZE);
    std::mt19937 rng(20261117);
    for (size_t i = 0; i < PAYLOAD_SIZE; ++i) {
        payload[i] = static_cast<uint8_t>(rng() & 0xFF);
    }

    uint16_t tracker_port = 0;
    uint16_t peer_port    = 0;
    int tracker_listen_fd = create_listen_socket(tracker_port);
    int peer_listen_fd    = create_listen_socket(peer_port);

    std::string announce_url = "http://127.0.0.1:" + std::to_string(tracker_port) + "/announce";
    std::string raw_torrent;
    bt::Metainfo meta = make_synthetic_torrent(announce_url, payload, PIECE_LEN, raw_torrent);

    std::atomic<bool> stop_servers{false};

    // HTTP Tracker server thread
    std::thread tracker_srv([&]() {
        while (!stop_servers.load()) {
            struct sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            int cfd = ::accept(tracker_listen_fd, reinterpret_cast<struct sockaddr*>(&cli), &clen);
            if (cfd < 0) break;

            char req_buf[1024];
            (void)::recv(cfd, req_buf, sizeof(req_buf), 0);

            // Build 6-byte compact peer entry for 127.0.0.1:<peer_port>
            std::string compact_peers(6, '\0');
            uint32_t ip_be   = htonl(INADDR_LOOPBACK);
            uint16_t port_be = htons(peer_port);
            std::memcpy(&compact_peers[0], &ip_be,   4);
            std::memcpy(&compact_peers[4], &port_be, 2);

            bt::BDict resp_dict;
            resp_dict["interval"]   = bt::BValue(int64_t(60));
            resp_dict["complete"]   = bt::BValue(int64_t(1));
            resp_dict["incomplete"] = bt::BValue(int64_t(0));
            resp_dict["peers"]      = bt::BValue(compact_peers);

            std::string body = bt::encode(bt::BValue(resp_dict));
            std::string http_resp =
                "HTTP/1.0 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

            send_all(cfd, http_resp.data(), http_resp.size());
            ::close(cfd);
        }
    });

    // BEP 3 Peer Seeder thread
    std::thread peer_srv([&]() {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int cfd = ::accept(peer_listen_fd, reinterpret_cast<struct sockaddr*>(&cli), &clen);
        if (cfd < 0) return;

        // 1. Read client 68-byte handshake and reply with ours
        uint8_t hs[68];
        if (!recv_all(cfd, hs, 68)) { ::close(cfd); return; }
        std::memcpy(hs + 48, "-SEED01-123456789012", 20);
        if (!send_all(cfd, hs, 68)) { ::close(cfd); return; }

        // 2. Send Bitfield (8 pieces = 1 byte 0xFF)
        uint8_t bf_msg[6] = {0, 0, 0, 2, 5, 0xFF};
        send_all(cfd, bf_msg, sizeof(bf_msg));

            // 3. Process client messages
        while (!stop_servers.load()) {
            uint8_t len_buf[4];
            if (!recv_all(cfd, len_buf, 4)) break;
            uint32_t msg_len = read_u32_be(len_buf);
            if (msg_len == 0) continue; // keep-alive

            std::vector<uint8_t> msg(msg_len);
            if (!recv_all(cfd, msg.data(), msg_len)) break;

            uint8_t type = msg[0];
            if (type == 2) {
                // Client is Interested -> send Unchoke (type = 1)
                uint8_t unchoke[5] = {0, 0, 0, 1, 1};
                if (!send_all(cfd, unchoke, sizeof(unchoke))) break;
            } else if (type == 6 && msg_len == 13) {
                // Request: piece_index (4B), begin (4B), length (4B)
                uint32_t piece_idx = read_u32_be(&msg[1]);
                uint32_t begin     = read_u32_be(&msg[5]);
                uint32_t length    = read_u32_be(&msg[9]);

                size_t global_off = static_cast<size_t>(piece_idx) * PIECE_LEN + begin;
                if (global_off + length <= payload.size()) {
                    std::vector<uint8_t> piece_msg(4 + 1 + 8 + length);
                    write_u32_be(&piece_msg[0], 9 + length);
                    piece_msg[4] = 7; // MsgType::Piece
                    write_u32_be(&piece_msg[5], piece_idx);
                    write_u32_be(&piece_msg[9], begin);
                    std::memcpy(&piece_msg[13], payload.data() + global_off, length);
                    if (!send_all(cfd, piece_msg.data(), piece_msg.size())) break;
                }
            }
        }
        ::close(cfd);
    });

    std::printf("[TEST 5/5] Running full bt::Session download over TCP...\n");

    std::string tmp_dir = "/tmp/ps4_torrent_e2e_" + std::to_string(::getpid());
    std::string torrent_file = tmp_dir + ".torrent";
    {
        std::ofstream ofs(torrent_file, std::ios::binary);
        ofs.write(raw_torrent.data(), static_cast<std::streamsize>(raw_torrent.size()));
    }

    bt::Session session(torrent_file, tmp_dir);
    float last_pct = 0.0f;
    session.set_progress_callback([&](const bt::ProgressInfo& info) {
        last_pct = info.pct;
    });

    session.start();

    TEST_ASSERT(session.is_complete(), "Session did not complete");
    TEST_ASSERT(last_pct == 100.0f, "Final progress callback was not 100%");

    // Verify both reconstructed files on disk byte-for-byte against original payload
    std::string f1_path = tmp_dir + "/test_bundle/sub/part1.bin";
    std::string f2_path = tmp_dir + "/test_bundle/part2.bin";

    std::ifstream ifs1(f1_path, std::ios::binary);
    std::ifstream ifs2(f2_path, std::ios::binary);
    TEST_ASSERT(ifs1.good() && ifs2.good(), "Downloaded multi-file outputs missing on disk");

    std::vector<uint8_t> downloaded;
    downloaded.insert(downloaded.end(),
                      std::istreambuf_iterator<char>(ifs1),
                      std::istreambuf_iterator<char>());
    downloaded.insert(downloaded.end(),
                      std::istreambuf_iterator<char>(ifs2),
                      std::istreambuf_iterator<char>());

    TEST_ASSERT(downloaded.size() == payload.size(), "Downloaded total byte count mismatch");
    TEST_ASSERT(std::memcmp(downloaded.data(), payload.data(), payload.size()) == 0,
                "Downloaded file contents do not match original payload byte-for-byte!");

    // Clean up servers and temp files
    stop_servers.store(true);
    ::shutdown(tracker_listen_fd, SHUT_RDWR);
    ::close(tracker_listen_fd);
    ::shutdown(peer_listen_fd, SHUT_RDWR);
    ::close(peer_listen_fd);
    if (tracker_srv.joinable()) tracker_srv.join();
    if (peer_srv.joinable())    peer_srv.join();

    ::unlink(torrent_file.c_str());
    std::string rm_cmd = "rm -rf " + tmp_dir;
    std::system(rm_cmd.c_str());

    std::printf("  -> PASS (256 KiB downloaded, SHA1-verified, and byte-for-byte identical)\n");
}

} // anonymous namespace

int main() {
    std::printf("============================================================\n");
    std::printf("  PS4 Torrent — Automated Verification Suite\n");
    std::printf("============================================================\n");

    test_sha1();
    test_bencode();
    test_piece_manager();
    test_end_to_end_session();

    std::printf("============================================================\n");
    std::printf("  ALL TESTS PASSED SUCCESSFULLY!\n");
    std::printf("============================================================\n");
    return 0;
}
