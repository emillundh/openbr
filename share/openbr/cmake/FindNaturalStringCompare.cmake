find_path(NATURALSTRINGCOMPARE_DIR NaturalStringCompare.h ${CMAKE_SOURCE_DIR}/3rdparty/*)

include_directories(${NATURALSTRINGCOMPARE_DIR})
set(NATURALSTRINGCOMPARE_SRC ${NATURALSTRINGCOMPARE_DIR}/NaturalStringCompare.cpp)
