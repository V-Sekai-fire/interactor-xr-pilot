# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.24)
# Writes INPUT as a C++ byte array named SYMBOL into OUTPUT.
file(READ ${INPUT} hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR bytes "${hexlen} / 2")
set(pair "[0-9a-f][0-9a-f]")
set(row "${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}${pair}")
string(REGEX REPLACE "(${row})" "\\1\n" hex "${hex}")
string(REGEX REPLACE "(${pair})" "0x\\1," hex "${hex}")
file(WRITE ${OUTPUT} "// Generated from ${INPUT}\n#include <cstddef>\nnamespace panelspun::embedded {\nextern const unsigned char ${SYMBOL}[] = {\n${hex}\n};\nextern const std::size_t ${SYMBOL}_size = ${bytes};\n}\n")
