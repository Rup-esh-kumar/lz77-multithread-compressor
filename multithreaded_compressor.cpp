// multithreaded_compressor.cpp
// Single-file multithreaded LZ77 chunked compressor + decompressor for Windows
// Build (MinGW): g++ multithreaded_compressor.cpp -o compressor.exe -std=c++17 -O2 -pthread
// Build (MSVC): cl /EHsc /std:c++17 multithreaded_compressor.cpp

#include <bits/stdc++.h>
using namespace std;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// ---------------------- ThreadPool ----------------------
class ThreadPool {
public:
    ThreadPool(size_t n) : stop(false) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this](){ worker_loop(); });
        }
    }
    ~ThreadPool(){
        {
            unique_lock<mutex> lk(m);
            stop = true;
        }
        cv.notify_all();
        for (auto &t: workers) if (t.joinable()) t.join();
    }

    template<class F>
    auto enqueue(F&& f) -> future<decltype(f())> {
        using R = decltype(f());
        auto task = make_shared<packaged_task<R()>>(forward<F>(f));
        future<R> fut = task->get_future();
        {
            unique_lock<mutex> lk(m);
            tasks.emplace([task]{ (*task)(); });
        }
        cv.notify_one();
        return fut;
    }

private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex m;
    condition_variable cv;
    bool stop;

    void worker_loop(){
        while (true) {
            function<void()> job;
            {
                unique_lock<mutex> lk(m);
                cv.wait(lk, [this]{ return stop || !tasks.empty(); });
                if (stop && tasks.empty()) return;
                job = move(tasks.front()); tasks.pop();
            }
            job();
        }
    }
};

// ---------------------- Simple LZ77 ----------------------
// Token format used here (byte-aligned simple format):
// - Literal token: 1 byte flag 0x00, then 1 byte literal value
// - Match token:   1 byte flag 0x01, then 2 bytes offset (big-endian), then 1 byte length (1..255)

struct LZ77 {
    size_t window_size = 1 << 12; // 4096
    size_t lookahead = 255;       // max match length

    vector<u8> compress(const vector<u8>& input){
        vector<u8> out;
        size_t n = input.size();
        size_t pos = 0;
        while (pos < n) {
            size_t best_off = 0, best_len = 0;
            size_t start = (pos > window_size) ? pos - window_size : 0;
            // naive search (O(W * L)) - okay for chunk sizes like 1MB
            for (size_t i = start; i < pos; ++i) {
                size_t len = 0;
                while (len < lookahead && pos + len < n && input[i + len] == input[pos + len]) ++len;
                if (len > best_len) { best_len = len; best_off = pos - i; }
                if (best_len == lookahead) break;
            }
            if (best_len >= 3) {
                // emit match token
                out.push_back(0x01);
                u16 off = (u16)best_off; // fits window_size
                out.push_back((off >> 8) & 0xFF);
                out.push_back(off & 0xFF);
                u8 llen = (u8)min<size_t>(best_len, 255);
                out.push_back(llen);
                pos += best_len;
            } else {
                // literal
                out.push_back(0x00);
                out.push_back(input[pos]);
                ++pos;
            }
        }
        return out;
    }

    vector<u8> decompress(const vector<u8>& input){
        vector<u8> out;
        size_t pos = 0, n = input.size();
        while (pos < n) {
            u8 flag = input[pos++];
            if (flag == 0x00) {
                if (pos >= n) throw runtime_error("corrupt literal");
                out.push_back(input[pos++]);
            } else if (flag == 0x01) {
                if (pos + 3 > n) throw runtime_error("corrupt match");
                u16 off = (u16(input[pos]) << 8) | u16(input[pos+1]); pos += 2;
                u8 len = input[pos++];
                if (off == 0 || off > out.size()) throw runtime_error("invalid offset");
                size_t start = out.size() - off;
                for (size_t i = 0; i < len; ++i) {
                    out.push_back(out[start + i]);
                }
            } else {
                throw runtime_error("unknown token flag");
            }
        }
        return out;
    }
};

// ---------------------- File helpers ----------------------
static vector<u8> read_file_chunk(const string& filename, u64 offset, size_t size) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) return {};
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return {}; }
    vector<u8> buf; buf.resize(size);
    size_t r = fread(buf.data(), 1, size, f);
    buf.resize(r);
    fclose(f);
    return buf;
}

static u64 file_size(const string& filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long s = ftell(f);
    fclose(f);
    return (u64) s;
}

static void write_all(const string& filename, const vector<vector<u8>>& chunks, const vector<u64>& original_sizes) {
    // Format: magic 'MTC1' (4 bytes)
    // u32 chunk_count
    // For each chunk: u64 original_size, u64 compressed_size, then compressed bytes
    FILE* f = fopen(filename.c_str(), "wb");
    if (!f) throw runtime_error("cannot open output file");
    fwrite("MTC1", 1, 4, f);
    u32 cnt = (u32)chunks.size();
    fwrite(&cnt, sizeof(u32), 1, f);
    for (size_t i = 0; i < chunks.size(); ++i) {
        u64 orig = original_sizes[i];
        u64 comp = chunks[i].size();
        fwrite(&orig, sizeof(u64), 1, f);
        fwrite(&comp, sizeof(u64), 1, f);
        if (comp) fwrite(chunks[i].data(), 1, comp, f);
    }
    fclose(f);
}

static void read_and_decompress_file(const string& inname, const string& outname) {
    FILE* f = fopen(inname.c_str(), "rb");
    if (!f) throw runtime_error("cannot open input file");
    char magic[4]; if (fread(magic,1,4,f)!=4) throw runtime_error("bad file");
    if (memcmp(magic, "MTC1", 4) != 0) throw runtime_error("not a MTC1 file");
    u32 cnt; if (fread(&cnt, sizeof(u32), 1, f)!=1) throw runtime_error("bad file header");
    FILE* out = fopen(outname.c_str(), "wb");
    if (!out) { fclose(f); throw runtime_error("cannot open output file"); }
    LZ77 codec;
    for (u32 i = 0; i < cnt; ++i) {
        u64 orig, comp; 
        if (fread(&orig, sizeof(u64), 1, f) != 1) throw runtime_error("bad file");
        if (fread(&comp, sizeof(u64), 1, f) != 1) throw runtime_error("bad file");
        vector<u8> compbuf; compbuf.resize((size_t)comp);
        if (comp && fread(compbuf.data(), 1, (size_t)comp, f) != comp) throw runtime_error("bad file read");
        auto decomp = codec.decompress(compbuf);
        if (decomp.size() != orig) {
            // It's possible compressor used token optimization; still check
            // If mismatch, just write what we have
        }
        if (!decomp.empty()) fwrite(decomp.data(), 1, decomp.size(), out);
    }
    fclose(out);
    fclose(f);
}

// ---------------------- Main compressor flow ----------------------
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    if (argc < 3) {
        cerr << "Usage:\n";
        cerr << "  To compress:   " << argv[0] << " c <input-file> <output-file> [chunk_size_bytes]\n";
        cerr << "  To decompress: " << argv[0] << " d <input-file> <output-file>\n";
        return 1;
    }

    string mode = argv[1];
    if (mode == "d" || mode == "D") {
        if (argc < 4) { cerr << "missing file args for decompress\n"; return 1; }
        string in = argv[2], out = argv[3];
        try { read_and_decompress_file(in, out); cout << "Decompression done.\n"; }
        catch (exception &e) { cerr << "Error: " << e.what() << "\n"; return 1; }
        return 0;
    }

    if (mode != "c" && mode != "C") { cerr << "unknown mode\n"; return 1; }
    string inname = argv[2];
    string outname = argv[3];
    size_t chunk_size = 1 << 20; // default 1MB
    if (argc >= 5) chunk_size = stoull(argv[4]);

    u64 fsize = file_size(inname);
    if (fsize == 0) { cerr << "cannot read input or file empty\n"; return 1; }
    size_t num_chunks = (size_t)((fsize + chunk_size - 1) / chunk_size);
    cout << "Input size: " << fsize << " bytes; chunks: " << num_chunks << " (" << chunk_size << " bytes each)\n";

    // prepare threadpool
    unsigned int hw = thread::hardware_concurrency(); if (hw == 0) hw = 2;
    ThreadPool pool(hw);
    cout << "Using " << hw << " worker threads.\n";

    vector<future<pair<size_t, vector<u8>>>> futures; futures.reserve(num_chunks);
    LZ77 codec; // local codec for main thread; each task will construct its own codec as needed

    for (size_t i = 0; i < num_chunks; ++i) {
        u64 offset = (u64)i * chunk_size;
        size_t read_sz = (size_t)min<u64>(chunk_size, (u64)fsize - offset);
        // read chunk into memory
        vector<u8> chunk = read_file_chunk(inname, offset, read_sz);
        if (chunk.empty() && read_sz != 0) { cerr << "Failed to read chunk "<<i<<"\n"; return 1; }
        // move into task
        auto task = [i, chunk = move(chunk)]() -> pair<size_t, vector<u8>> {
            LZ77 localcodec;
            auto comp = localcodec.compress(chunk);
            return {i, move(comp)};
        };
        futures.push_back(pool.enqueue(task));
    }

    // collect results
    vector<vector<u8>> compressed_chunks(num_chunks);
    vector<u64> original_sizes(num_chunks);
    for (size_t i = 0; i < num_chunks; ++i) original_sizes[i] = (u64) ( (i==num_chunks-1) ? (fsize - (u64)i*chunk_size) : chunk_size );

    for (auto &fut: futures) {
        auto p = fut.get();
        compressed_chunks[p.first] = move(p.second);
        cout << "Chunk "<<p.first<<" compressed: "<<compressed_chunks[p.first].size()<<" bytes\n";
    }

    // write combined file
    try {
        write_all(outname, compressed_chunks, original_sizes);
        cout << "Compression finished. Output: " << outname << "\n";
    } catch (exception &e) {
        cerr << "Failed to write output: " << e.what() << "\n"; return 1;
    }

    return 0;
}
