#include <ncurses.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <sstream>
#include <unistd.h>

bool is_music_file(const std::string &filename) {
    std::string ext;
    auto pos = filename.find_last_of('.');
    if (pos == std::string::npos) return false;
    ext = filename.substr(pos + 1);
    return ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" || ext == "m4a";
}

bool is_url(const std::string& path) {
    return path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
}

std::string run_command(const std::string& cmd) {
    std::string result;
    char buffer[128];
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

std::vector<std::string> get_remote_music_files(const std::string& url, std::string& auth) {
    std::vector<std::string> files;
    std::string curl_cmd = "curl -s " + auth + " \"" + url + "\"";
    std::string html = run_command(curl_cmd);

    if (html.find("401 Authorization Required") != std::string::npos) {
        endwin();
        std::string user, pass;
        std::cout << "Username: ";
        std::getline(std::cin, user);
        std::cout << "Password: ";
        system("stty -echo");
        std::getline(std::cin, pass);
        system("stty echo");
        std::cout << std::endl;
        auth = "-u " + user + ":" + pass;
        return get_remote_music_files(url, auth);
    }

    std::regex href_regex(R"delim(<a\s+href\s*=\s*"([^"]+\.(mp3|wav|flac|ogg|m4a))")delim", std::regex::icase);
    std::smatch match;
    std::string::const_iterator searchStart(html.cbegin());

    while (std::regex_search(searchStart, html.cend(), match, href_regex)) {
        std::string filename = match[1];
        size_t q = filename.find('?');
        if (q != std::string::npos) filename = filename.substr(0, q);
        files.push_back(filename);
        searchStart = match.suffix().first;
    }

    return files;
}

std::vector<std::string> get_local_music_files(const std::string& dirpath) {
    std::vector<std::string> files;
    DIR *dir = opendir(dirpath.c_str());
    if (!dir) return files;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (entry->d_type == DT_REG && is_music_file(name)) {
            files.push_back(name);
        }
    }
    closedir(dir);
    return files;
}

std::string url_decode(const std::string& str) {
    std::ostringstream decoded;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '%' && i + 2 < str.size()) {
            std::string hex = str.substr(i + 1, 2);
            decoded << (char)std::stoi(hex, nullptr, 16);
            i += 2;
        } else if (str[i] == '+') {
            decoded << ' ';
        } else {
            decoded << str[i];
        }
    }
    return decoded.str();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <music_directory_or_url>\n";
        return 1;
    }

    std::string path = argv[1];
    std::vector<std::string> files;
    std::string auth;

    if (is_url(path)) {
        files = get_remote_music_files(path, auth);
    } else {
        files = get_local_music_files(path);
    }

    if (files.empty()) {
        std::cerr << "No music files found in " << path << "\n";
        return 1;
    }

    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);

    int highlight = 0, ch;
    int start_index = 0;

    while (true) {
        clear();
        int height, width;
        getmaxyx(stdscr, height, width);

        mvprintw(0, 0, "Music Player - %s (Arrow keys to navigate, Enter to play, q to quit)", path.c_str());
        int max_display = height - 3;

        if (highlight < start_index) start_index = highlight;
        else if (highlight >= start_index + max_display) start_index = highlight - max_display + 1;

        for (int i = 0; i < max_display && (start_index + i) < (int)files.size(); i++) {
            int idx = start_index + i;
            std::string decoded_name = url_decode(files[idx]);
            if (idx == highlight) attron(A_REVERSE);
            mvprintw(i + 2, 0, "%s", decoded_name.c_str());
            if (idx == highlight) attroff(A_REVERSE);
        }

        ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        else if (ch == KEY_UP && highlight > 0) highlight--;
        else if (ch == KEY_DOWN && highlight < (int)files.size() - 1) highlight++;
        else if (ch == 10) {
            endwin();
            std::string cmd;
            if (is_url(path)) {
                std::string full_url = path + (path.back() == '/' ? "" : "/") + files[highlight];
                if (auth.empty()) {
                    cmd = "mpv --no-video --quiet \"" + full_url + "\"";
                } else {
                    cmd = "curl -s -u " + auth.substr(3) + " \"" + full_url + "\" | mpv --no-video -";
                }
            } else {
                std::string filepath = path + "/" + files[highlight];
                cmd = "mpv --no-video --quiet \"" + filepath + "\"";
            }
            system(cmd.c_str());

            // Restart ncurses UI
            initscr();
            noecho();
            cbreak();
            keypad(stdscr, TRUE);
            curs_set(0);
        }
    }

    endwin();
    return 0;
}
