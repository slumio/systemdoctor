#include "streamer.h"
#include "../utils.h"
#include <iostream>

MdStreamer::MdStreamer() {}

bool MdStreamer::process_token() {
    if (utils::starts_with(buffer, "```")) {
        code_block = !code_block;
        if (code_block) std::cout << "\x1b[36m";
        else std::cout << "\x1b[0m";
        buffer = buffer.substr(3);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "$$") || utils::starts_with(buffer, "\\[") || utils::starts_with(buffer, "\\]")) {
        math_block = !math_block;
        if (math_block) std::cout << "\x1b[35m";
        else std::cout << "\x1b[0m";
        buffer = buffer.substr(2);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "\\(") || utils::starts_with(buffer, "\\)")) {
        math_inline = !math_inline;
        if (math_inline) std::cout << "\x1b[35m";
        else std::cout << "\x1b[0m";
        buffer = buffer.substr(2);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "**")) {
        bold = !bold;
        if (bold) std::cout << "\x1b[1m";
        else std::cout << "\x1b[22m";
        buffer = buffer.substr(2);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "### ") && is_newline) {
        std::cout << "\x1b[1;96m### ";
        buffer = buffer.substr(4);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "## ") && is_newline) {
        std::cout << "\x1b[1;96m## ";
        buffer = buffer.substr(3);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "# ") && is_newline) {
        std::cout << "\x1b[1;96m# ";
        buffer = buffer.substr(2);
        is_newline = false;
        return true;
    }
    if ((utils::starts_with(buffer, "- ") || utils::starts_with(buffer, "* ")) && is_newline) {
        std::cout << "\x1b[33m• \x1b[0m";
        buffer = buffer.substr(2);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "`")) {
        inline_code = !inline_code;
        if (inline_code) std::cout << "\x1b[36m";
        else std::cout << "\x1b[0m";
        buffer = buffer.substr(1);
        is_newline = false;
        return true;
    }
    if (utils::starts_with(buffer, "$")) {
        math_inline = !math_inline;
        if (math_inline) std::cout << "\x1b[35m";
        else std::cout << "\x1b[0m";
        buffer = buffer.substr(1);
        is_newline = false;
        return true;
    }
    
    return false;
}

void MdStreamer::print(const std::string& text) {
    buffer += text;
    
    while (buffer.length() >= 4) {
        if (!process_token()) {
            char c = buffer[0];
            buffer = buffer.substr(1);
            
            if (c == '\n') {
                is_newline = true;
                std::cout << "\x1b[0m";
                if (code_block) std::cout << "\x1b[36m";
                if (math_block) std::cout << "\x1b[35m";
                if (bold) std::cout << "\x1b[1m";
            } else {
                is_newline = false;
            }
            std::cout << c;
        }
    }
    std::cout.flush();
}

void MdStreamer::flush() {
    while (!buffer.empty()) {
        if (!process_token()) {
            char c = buffer[0];
            buffer = buffer.substr(1);
            
            if (c == '\n') {
                is_newline = true;
                std::cout << "\x1b[0m";
                if (code_block) std::cout << "\x1b[36m";
                if (math_block) std::cout << "\x1b[35m";
                if (bold) std::cout << "\x1b[1m";
            } else {
                is_newline = false;
            }
            std::cout << c;
        }
    }
    std::cout << "\x1b[0m"; // Reset all at the end
    std::cout.flush();
}
