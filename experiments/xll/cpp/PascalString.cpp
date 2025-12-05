#include "PascalString.h"
#include <vector>
#include <string>
#include <algorithm> // For std::min

// Converts a C-style string to a Pascal-style string (length-prefixed).
// The resulting string is null-terminated.
// Excel's Pascal strings are typically limited to 255 characters.
std::vector<char> CStringToPascalString(const std::string& c_str) {
    // Excel Pascal strings limit the length to 255.
    unsigned char length = static_cast<unsigned char>(std::min((size_t)255, c_str.length()));
    std::vector<char> pascal_str(length + 1); // +1 for the length byte

    pascal_str[0] = static_cast<char>(length);
    std::copy(c_str.begin(), c_str.begin() + length, pascal_str.begin() + 1);

    return pascal_str;
}

// Converts a Pascal-style string (length-prefixed) to a C-style string.
// Assumes the input is a wide character Pascal string (unsigned short*), typical for Excel12.
std::string PascalStringToCString(const unsigned short* pascal_str) {
    if (!pascal_str) {
        return "";
    }
    // The first unsigned short contains the length of the string
    unsigned short length = pascal_str[0];
    // The actual string data starts from the second unsigned short
    return std::string(pascal_str + 1, pascal_str + 1 + length);
}
