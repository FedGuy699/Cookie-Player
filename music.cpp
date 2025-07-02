#include <ncurses.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>

bool is_music_file(const std::string &filename) {
    std::string ext;
    auto pos = filename.find_last_of('.');
    if (pos == std::string::npos) return false;
    ext = filename.substr(pos + 1);
    return ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" || ext == "m4a";
}

std::vector<std::string> get_music_files(const std::string& dirpath) {
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <music_directory>\n";
        return 1;
    }

    std::string dirpath = argv[1];
    std::vector<std::string> files = get_music_files(dirpath);
    if (files.empty()) {
        std::cerr << "No music files found in " << dirpath << "\n";
        return 1;
    }

    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);

    int highlight = 0;
    int ch;

    int start_index = 0; 

    while (true) {
        clear();
        int height, width;
        getmaxyx(stdscr, height, width);

        mvprintw(0, 0, "Music Player - %s (Use arrow keys, Enter to play, q to quit)", dirpath.c_str());

        int max_display = height - 3;

        if (highlight < start_index) {
            start_index = highlight;
        } else if (highlight >= start_index + max_display) {
            start_index = highlight - max_display + 1;
        }

        for (int i = 0; i < max_display && (start_index + i) < (int)files.size(); i++) {
            int idx = start_index + i;
            if (idx == highlight) attron(A_REVERSE);
            mvprintw(i + 2, 0, "%s", files[idx].c_str());
            if (idx == highlight) attroff(A_REVERSE);
        }

        ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        else if (ch == KEY_UP) {
            if (highlight > 0) highlight--;
        } else if (ch == KEY_DOWN) {
            if (highlight < (int)files.size() - 1) highlight++;
        } else if (ch == 10) {
            endwin();
            std::string filepath = dirpath + "/" + files[highlight];
            std::string cmd = "mpv --no-video --quiet \"" + filepath + "\"";
            system(cmd.c_str());
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
