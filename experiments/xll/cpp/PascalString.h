#ifndef PASCAL_STRING_H
#define PASCAL_STRING_H

#include <string>
#include <vector>

// Converts a C-style string to a Pascal-style string (length-prefixed).
// The resulting string is null-terminated.
std::vector<char> CStringToPascalString(const std::string& c_str);

// Converts a Pascal-style string (length-prefixed) to a C-style string.
std::string PascalStringToCString(const unsigned short* pascal_str);

#endif // PASCAL_STRING_H
