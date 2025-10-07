#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <ncurses.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <curl/curl.h>
#include <regex>
#include <cctype>
#include <locale.h>
#include <codecvt>


#define COLOR_BG 0
#define COLOR_HEADER 1
#define COLOR_LIST 2
#define COLOR_HIGHLIGHT 3
#define COLOR_PLAYBACK 4
#define COLOR_PROGRESS 5
#define COLOR_INPUT 6

std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring ws(str.size(), L'\0');
    std::mbstowcs(&ws[0], str.c_str(), str.size());
    ws.resize(std::wcslen(ws.c_str()));
    return ws;
}


std::string url_decode(const std::string &value) {
    std::string result;
    char ch;
    int i, ii;
    for (i = 0; i < (int)value.length(); i++) {
        if (value[i] == '%') {
            if (i + 2 < (int)value.length() &&
                std::isxdigit(value[i+1]) && std::isxdigit(value[i+2])) {
                std::string hex = value.substr(i + 1, 2);
                ch = (char) std::stoi(hex, nullptr, 16);
                result += ch;
                i += 2;
            } else {
                result += '%';
            }
        } else if (value[i] == '+') {
            result += ' ';
        } else {
            result += value[i];
        }
    }
    return result;
}


bool is_music_file(const std::string &filename) {
    auto pos = filename.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = filename.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" || ext == "m4a";
}

std::vector<std::string> get_local_music_files(const std::string &dirpath) {
    std::vector<std::string> files;
    DIR* dir = opendir(dirpath.c_str());
    if (!dir) return files;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        std::string name = entry->d_name;
        if (entry->d_type == DT_REG && is_music_file(name)) {
            files.push_back(name);
        }
    }
    closedir(dir);
    return files;
}

static size_t curl_write_memory_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::vector<char>* buffer = (std::vector<char>*)userp;
    buffer->insert(buffer->end(), (char*)contents, (char*)contents + totalSize);
    return totalSize;
}

std::vector<std::string> get_remote_music_files(const std::string& url, const std::string& username, const std::string& password) {
    std::vector<std::string> files;
    CURL* curl = curl_easy_init();
    if (!curl) return files;

    std::vector<char> response_data;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

    if (!username.empty()) {
        std::string userpass = username + ":" + password;
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpass.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return files;
    }

    curl_easy_cleanup(curl);

    std::string html(response_data.begin(), response_data.end());

    std::regex href_regex(R"(<a\s+href=["']([^"'>]+)["'])", std::regex::icase);
    auto begin = std::sregex_iterator(html.begin(), html.end(), href_regex);
    auto end = std::sregex_iterator();

    for (auto i = begin; i != end; ++i) {
        std::string link = (*i)[1].str();
        if (link == "../" || link.back() == '/') continue;
        if (is_music_file(link)) {
            files.push_back(link);
        }
    }


    return files;
}

struct MemoryFile {
    const char* data;
    size_t size;
    size_t offset;
};

static ma_result memory_read(ma_decoder* pDecoder, void* pBuffer, size_t bytesToRead, size_t* bytesRead) {
    MemoryFile* mem = (MemoryFile*)pDecoder->pUserData;
    if (mem->offset + bytesToRead > mem->size) {
        bytesToRead = mem->size - mem->offset;
    }
    memcpy(pBuffer, mem->data + mem->offset, bytesToRead);
    mem->offset += bytesToRead;
    *bytesRead = bytesToRead;
    return MA_SUCCESS;
}

static ma_result memory_seek(ma_decoder* pDecoder, ma_int64 offset, ma_seek_origin origin) {
    MemoryFile* mem = (MemoryFile*)pDecoder->pUserData;
    int64_t newOffset = 0;

    if (origin == ma_seek_origin_start) {
        newOffset = offset;
    } else if (origin == ma_seek_origin_current) {
        newOffset = (int64_t)mem->offset + offset;
    } else if (origin == ma_seek_origin_end) {
        newOffset = (int64_t)mem->size + offset;
    }

    if (newOffset < 0 || newOffset > (int64_t)mem->size) return MA_INVALID_ARGS;

    mem->offset = (size_t)newOffset;
    return MA_SUCCESS;
}


std::vector<char> fetch_remote_file(const std::string& url, const std::string& username, const std::string& password) {
    std::vector<char> file_data;
    CURL* curl = curl_easy_init();
    if (!curl) return file_data;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file_data);

    if (!username.empty()) {
        std::string userpass = username + ":" + password;
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpass.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        file_data.clear();
        return file_data;
    }

    curl_easy_cleanup(curl);
    return file_data;
}

struct PlaybackState {
    ma_decoder decoder{};
    ma_device device{};
    std::atomic<bool> playing{false};
    std::atomic<bool> stop_requested{false};
    std::mutex mx;
    std::atomic<ma_uint64> current_frame{0};
    std::atomic<ma_uint64> total_frames{0};
    std::string current_file;
    std::atomic<bool> paused{false};

    std::atomic<bool> track_finished{false};


    std::vector<char> remote_file_data;
    MemoryFile mem_file{};
};

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* state = (PlaybackState*)pDevice->pUserData;
    if (!state) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(ma_format_f32, 2));
        return;
    }

    if (state->paused) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        return;
    }

    ma_uint64 framesRead = 0;
    ma_result result = ma_decoder_read_pcm_frames(&state->decoder, pOutput, frameCount, &framesRead);
    
    ma_uint64 cur;
    ma_decoder_get_cursor_in_pcm_frames(&state->decoder, &cur);
    state->current_frame = cur;

    if (result != MA_SUCCESS || framesRead < frameCount) {
        size_t bytesPerFrame = ma_get_bytes_per_frame(ma_format_f32, state->decoder.outputChannels);
        size_t bytesToClear = (frameCount - framesRead) * bytesPerFrame;
        char* p = (char*)pOutput;
        memset(p + framesRead * bytesPerFrame, 0, bytesToClear);
    }
}

bool start_playback(PlaybackState &s, const std::string &filepath, bool is_remote, const std::string& username = "", const std::string& password = "") {
    std::lock_guard<std::mutex> lock(s.mx);
    
    if (s.playing) {
        s.stop_requested = true;
        ma_device_stop(&s.device);
        ma_device_uninit(&s.device);
        ma_decoder_uninit(&s.decoder);
        s.remote_file_data.clear();
        s.playing = false;
    }

    ma_result result;
    if (is_remote) {
        s.remote_file_data = fetch_remote_file(filepath, username, password);
        if (s.remote_file_data.empty()) return false;

        s.mem_file.data = s.remote_file_data.data();
        s.mem_file.size = s.remote_file_data.size();
        s.mem_file.offset = 0;
        s.decoder.pUserData = &s.mem_file;

        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, 44100);
        result = ma_decoder_init(memory_read, memory_seek, &s.mem_file, &config, &s.decoder);
    } else {
        result = ma_decoder_init_file(filepath.c_str(), NULL, &s.decoder);
    }

    if (result != MA_SUCCESS) return false;

    s.current_file = url_decode(filepath.substr(filepath.find_last_of("/") + 1));


    ma_uint64 length;
    ma_decoder_get_length_in_pcm_frames(&s.decoder, &length);
    s.total_frames = length;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = s.decoder.outputFormat;
    cfg.playback.channels = s.decoder.outputChannels;
    cfg.sampleRate = s.decoder.outputSampleRate;
    cfg.dataCallback = data_callback;
    cfg.pUserData = &s;
    cfg.performanceProfile = ma_performance_profile_low_latency;

    result = ma_device_init(NULL, &cfg, &s.device);
    if (result != MA_SUCCESS) {
        ma_decoder_uninit(&s.decoder);
        s.remote_file_data.clear();
        return false;
    }

    result = ma_device_start(&s.device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&s.device);
        ma_decoder_uninit(&s.decoder);
        s.remote_file_data.clear();
        return false;
    }

    s.stop_requested = false;
    s.playing = true;
    s.paused = false;
    s.current_frame = 0;

    return true;
}

void stop_playback(PlaybackState &s) {
    std::lock_guard<std::mutex> lock(s.mx);
    if (!s.playing) return;

    s.stop_requested = true;
    s.playing = false;
    s.paused = false;
    ma_device_stop(&s.device);
    ma_device_uninit(&s.device);
    ma_decoder_uninit(&s.decoder);
    s.remote_file_data.clear();
}

void toggle_pause(PlaybackState &s) {
    std::lock_guard<std::mutex> lock(s.mx);
    if (!s.playing) return;
    s.paused = !s.paused;
}

void draw_separator(int y, int w) {
    attron(COLOR_PAIR(COLOR_HEADER));
    mvhline(y, 0, ACS_HLINE, w);
    attroff(COLOR_PAIR(COLOR_HEADER));
}

void draw_playback_bar(int h, int w, PlaybackState &state) {
    if (!state.playing) {
        attron(COLOR_PAIR(COLOR_PLAYBACK));
        mvprintw(h - 2, 0, "No track selected");
        mvprintw(h - 1, 0, "Press ENTER to play selected track");
        attroff(COLOR_PAIR(COLOR_PLAYBACK));
        return;
    }

    ma_uint64 cur = state.current_frame.load();
    ma_uint64 len = state.total_frames.load();

    if (len > 0) {
        double sampleRate = 44100;
        {
            std::lock_guard<std::mutex> lock(state.mx);
            if (state.playing && state.decoder.outputSampleRate > 0) {
                sampleRate = state.decoder.outputSampleRate;
            }
        }

        double pos_sec = double(cur) / sampleRate;
        double dur_sec = double(len) / sampleRate;
        int bar_w = w - 20;
        int filled = std::min(bar_w, static_cast<int>((pos_sec / dur_sec) * bar_w));
        
        attron(COLOR_PAIR(COLOR_PLAYBACK));
        std::string decoded_name = url_decode(state.current_file);
        mvprintw(h - 2, 0, "%s %s", state.paused ? "Paused - " : "Playing - ", decoded_name.c_str());

        
        attron(COLOR_PAIR(COLOR_PROGRESS));
        mvprintw(h - 1, 0, "[%.*s%*s] %02d:%02d / %02d:%02d",
                filled, std::string(filled, '=').c_str(),
                bar_w - filled, "",
                int(pos_sec) / 60, int(pos_sec) % 60,
                int(dur_sec) / 60, int(dur_sec) % 60);
        attroff(COLOR_PAIR(COLOR_PROGRESS));
    }
}

void draw_header(int w, const std::string &path) {
    attron(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
    mvprintw(0, 0, "COOKIE PLAYER");
    mvprintw(0, w - (int)path.length() - 1, "%s", path.c_str());
    attroff(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
}

void draw_footer(int h, int w) {
    attron(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
    mvprintw(h - 3, 0, "Controls: UP/DOWN Navigate | ENTER Play | SPACE Pause | q Quit");
    attroff(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
}

std::string get_input(const std::string &prompt, bool hide_input = false) {
    echo();
    curs_set(1);
    
    int h, w;
    getmaxyx(stdscr, h, w);
    
    attron(COLOR_PAIR(COLOR_INPUT) | A_BOLD);
    mvprintw(h - 1, 0, "%s", prompt.c_str());
    clrtoeol();
    attroff(COLOR_PAIR(COLOR_INPUT) | A_BOLD);
    
    if (hide_input) noecho();
    
    char input[256];
    getnstr(input, sizeof(input) - 1);
    
    if (hide_input) echo();
    curs_set(0);
    
    return std::string(input);
}

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "");
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();

    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    init_pair(COLOR_BG, COLOR_WHITE, COLOR_BLACK);
    init_pair(COLOR_HEADER, COLOR_CYAN, COLOR_BLACK);
    init_pair(COLOR_LIST, COLOR_WHITE, COLOR_BLACK);
    init_pair(COLOR_HIGHLIGHT, COLOR_BLACK, COLOR_WHITE);
    init_pair(COLOR_PLAYBACK, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_PROGRESS, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_INPUT, COLOR_MAGENTA, COLOR_BLACK);

    std::string path;
    if (argc < 2) {
        path = get_input("Enter music directory path or URL: ");
    } else {
        path = argv[1];
    }

    bool is_url = (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0);

    std::string username, password;
    if (is_url) {
        username = get_input("Username (leave empty if none): ");
        if (!username.empty()) {
            password = get_input("Password: ", true);
        }
    }

    std::vector<std::string> files;
    if (is_url) {
        files = get_remote_music_files(path, username, password);
    } else {
        files = get_local_music_files(path);
    }
    
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        std::string A = a, B = b;
        std::transform(A.begin(), A.end(), A.begin(), ::tolower);
        std::transform(B.begin(), B.end(), B.begin(), ::tolower);
        return A < B;
    });


    if (files.empty()) {
        endwin();
        std::cerr << "No music files found in " << path << "\n";
        curl_global_cleanup();
        return 1;
    }

    int highlight = 0, ch, start_idx = 0;
    PlaybackState state;
    std::thread pb_thread;

    auto cleanup_thread = [&]() {
        stop_playback(state);
        if (pb_thread.joinable()) pb_thread.join();
    };

    halfdelay(1);

    while (true) {
        erase();
        int h, w;
        getmaxyx(stdscr, h, w);

        draw_header(w, path);
        draw_separator(1, w);

        int list_height = h - 7;

        if (highlight < start_idx) start_idx = highlight;
        else if (highlight >= start_idx + list_height) start_idx = highlight - list_height + 1;

        for (int i = 0; i < list_height && start_idx + i < (int)files.size(); i++) {
            int idx = start_idx + i;
            std::string display_name = url_decode(files[idx]); 

            if (idx == highlight) {
                attron(COLOR_PAIR(COLOR_HIGHLIGHT) | A_BOLD);
            } else {
                attron(COLOR_PAIR(COLOR_LIST));
            }

            std::string prefix = (state.playing && display_name == state.current_file) ? "~ " : "  ";
            std::wstring wname = utf8_to_wstring(prefix + display_name);
            mvaddwstr(i + 2, 0, wname.c_str());


            if (idx == highlight) {
                attroff(COLOR_PAIR(COLOR_HIGHLIGHT) | A_BOLD);
            } else {
                attroff(COLOR_PAIR(COLOR_LIST));
            }
        }

        draw_separator(h - 5, w);
        draw_footer(h, w);
        draw_separator(h - 4, w);
        draw_playback_bar(h, w, state);

        wnoutrefresh(stdscr);
        doupdate();

        ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        else if (ch == KEY_UP && highlight > 0) highlight--;
        else if (ch == KEY_DOWN && highlight < (int)files.size() - 1) highlight++;
        else if (ch == 10) {
            if (state.playing && files[highlight] == state.current_file) {
                toggle_pause(state);
            } else {
                cleanup_thread();

                std::string fp;
                if (is_url) {
                    if (path.back() != '/') fp = path + "/";
                    else fp = path;
                    fp += files[highlight];
                    if (!username.empty()) {
                        start_playback(state, fp, true, username, password);
                    } else {
                        start_playback(state, fp, true);
                    }
                } else {
                    fp = path + "/" + files[highlight];
                    start_playback(state, fp, false);
                }

                pb_thread = std::thread([&]() {
                    while (state.playing && !state.stop_requested) {
                        ma_uint64 cur = state.current_frame.load();
                        ma_uint64 len = state.total_frames.load();
                        if (len > 0 && cur >= len) {
                            state.track_finished = true;
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                });
            }
        } else if (ch == ' ') {
            if (state.playing) {
                toggle_pause(state);
            }
        }

        if (state.track_finished) {
            state.track_finished = false;
            cleanup_thread(); 

            highlight = (highlight + 1) % files.size();

            std::string next_fp;
            if (is_url) {
                next_fp = path + (path.back() == '/' ? "" : "/") + files[highlight];
                if (!username.empty()) {
                    start_playback(state, next_fp, true, username, password);
                } else {
                    start_playback(state, next_fp, true);
                }
            } else {
                next_fp = path + "/" + files[highlight];
                start_playback(state, next_fp, false);
            }

            pb_thread = std::thread([&]() {
                while (state.playing && !state.stop_requested) {
                    ma_uint64 cur = state.current_frame.load();
                    ma_uint64 len = state.total_frames.load();
                    if (len > 0 && cur >= len) {
                        state.track_finished = true;
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }


    cleanup_thread();
    endwin();
    curl_global_cleanup();
    return 0;
}
