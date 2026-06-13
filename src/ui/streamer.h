#ifndef STREAMER_H
#define STREAMER_H

#include <string>

class MdStreamer {
private:
    std::string buffer = "";
    bool bold = false;
    bool code_block = false;
    bool inline_code = false;
    bool math_block = false;
    bool math_inline = false;
    bool is_newline = true;

    bool process_token();

public:
    MdStreamer();
    void print(const std::string& text);
    void flush();
};

#endif // STREAMER_H
