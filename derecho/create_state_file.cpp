/**
 * @file create_state_file.cpp
 * A small executable that reads a string representation of a View from stdin
 * and creates a serialized View file (readable by ManagedGroup) containing the
 * equivalent data. Basically the inverse of parse_state_file.
 *
 */

#include <iostream>
#include <string>

#include "persistence.h"
#include "view.h"

int main(int argc, char* argv[]) {
    if(argc < 2) {
        std::cout << "Usage: create_state_file <filename>" << std::endl;
        return 1;
    }

    std::string view_file_name(argv[1]);
    derecho::View view = derecho::parse_view(std::cin);
    derecho::persist_object(view, view_file_name);
    return 0;
}
