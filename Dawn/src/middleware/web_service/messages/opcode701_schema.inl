// Pinned build 86657, opcode 701 (80807603), reflected from the native descriptors.
// Offsets are within its 0x9C0 account-update body, starting at account + 0x748.
// Fixed arrays retain per-element presence bits. Unrelated fields are consumed only.
constexpr std::array<Field, 1> schema80807838{{
    {0, 128, 8, 64, 8, true, 0x00000000U, {}},
}};
constexpr std::array<Field, 1> schema808057E5{{
    {0, 13, 4, 32, 4, false, 0x00000000U, {}},
}};
constexpr std::array<Field, 2> schema80807837{{
    {0, 1, 0, 0, 0, true, 0x00000000U, schema80807838},
    {1024, 1, 0, 0, 0, true, 0x00000000U, schema808057E5},
}};
constexpr std::array<Field, 1> schema80807836{{
    {0, 2, 4, 32, 4, false, 0x00000000U, {}},
}};
constexpr std::array<Field, 1> schema80807835{{
    {0, 2, 8, 0, 0, true, 0x00000000U, schema80807836},
}};
constexpr std::array<Field, 1> schema80807A77{{
    {0, 50, 4, 32, 4, true, 0x80000000U, {}},
}};
constexpr std::array<Field, 1> schema80802FB7{{
    {0, 3, 200, 0, 0, false, 0x00000000U, schema80807A77},
}};
constexpr std::array<Field, 63> schema80807827{{
    {0, 1, 0, 1, 1, true, 0x00000000U, {}},
    {4, 1, 0, 32, 4, true, 0x80000000U, {}},
    {8, 1, 0, 4, 1, true, 0x00000001U, {}},
    {9, 1, 0, 3, 1, true, 0x00000001U, {}},
    {10, 1, 0, 4, 1, true, 0x00000001U, {}},
    {11, 1, 0, 3, 1, true, 0x00000001U, {}},
    {12, 1, 0, 32, 4, true, 0x80000000U, {}},
    {16, 1, 0, 32, 4, true, 0x00000000U, {}},
    {20, 1, 0, 2, 1, true, 0x00000001U, {}},
    {21, 1, 0, 4, 1, true, 0x00000001U, {}},
    {22, 1, 0, 4, 1, true, 0x00000001U, {}},
    {23, 1, 0, 4, 1, true, 0x00000001U, {}},
    {24, 1, 0, 4, 1, true, 0x00000001U, {}},
    {25, 1, 0, 4, 1, true, 0x00000001U, {}},
    {26, 1, 0, 4, 1, true, 0x00000001U, {}},
    {27, 1, 0, 2, 1, true, 0x00000001U, {}},
    {28, 1, 0, 2, 1, true, 0x00000001U, {}},
    {29, 1, 0, 3, 1, true, 0x00000001U, {}},
    {30, 1, 0, 2, 1, true, 0x00000001U, {}},
    {31, 1, 0, 3, 1, true, 0x00000001U, {}},
    {32, 1, 0, 3, 1, true, 0x00000001U, {}},
    {33, 1, 0, 2, 1, true, 0x00000001U, {}},
    {34, 1, 0, 3, 1, true, 0x00000001U, {}},
    {35, 1, 0, 4, 1, true, 0x00000001U, {}},
    {36, 1, 0, 4, 1, true, 0x00000001U, {}},
    {37, 1, 0, 4, 1, true, 0x00000001U, {}},
    {38, 1, 0, 4, 1, true, 0x00000001U, {}},
    {39, 1, 0, 4, 1, true, 0x00000001U, {}},
    {40, 1, 0, 1, 1, true, 0x00000000U, {}},
    {41, 1, 0, 1, 1, true, 0x00000000U, {}},
    {42, 1, 0, 1, 1, true, 0x00000000U, {}},
    {43, 1, 0, 1, 1, true, 0x00000000U, {}},
    {44, 1, 0, 1, 1, true, 0x00000000U, {}},
    {45, 1, 0, 1, 1, true, 0x00000000U, {}},
    {46, 1, 0, 1, 1, true, 0x00000000U, {}},
    {47, 1, 0, 1, 1, true, 0x00000000U, {}},
    {48, 1, 0, 1, 1, true, 0x00000000U, {}},
    {49, 1, 0, 1, 1, true, 0x00000000U, {}},
    {50, 1, 0, 1, 1, true, 0x00000000U, {}},
    {51, 1, 0, 1, 1, true, 0x00000000U, {}},
    {52, 1, 0, 1, 1, true, 0x00000000U, {}},
    {53, 1, 0, 1, 1, true, 0x00000000U, {}},
    {54, 1, 0, 1, 1, true, 0x00000000U, {}},
    {55, 1, 0, 1, 1, true, 0x00000000U, {}},
    {56, 1, 0, 2, 1, true, 0x00000001U, {}},
    {57, 1, 0, 1, 1, true, 0x00000000U, {}},
    {58, 1, 0, 1, 1, true, 0x00000000U, {}},
    {59, 1, 0, 3, 1, true, 0x00000001U, {}},
    {60, 1, 0, 3, 1, true, 0x00000001U, {}},
    {61, 1, 0, 1, 1, true, 0x00000000U, {}},
    {62, 1, 0, 2, 1, true, 0x00000001U, {}},
    {63, 1, 0, 2, 1, true, 0x00000001U, {}},
    {64, 1, 0, 2, 1, true, 0x00000001U, {}},
    {65, 1, 0, 2, 1, true, 0x00000001U, {}},
    {66, 1, 0, 2, 1, true, 0x00000001U, {}},
    {68, 1, 0, 32, 4, true, 0x00000000U, {}},
    {72, 1, 0, 32, 4, true, 0x00000000U, {}},
    {76, 1, 0, 3, 1, true, 0x00000001U, {}},
    {77, 1, 0, 2, 1, true, 0x00000001U, {}},
    {78, 1, 0, 1, 1, true, 0x00000000U, {}},
    {79, 1, 0, 1, 1, true, 0x00000000U, {}},
    {80, 1, 0, 1, 1, true, 0x00000000U, {}},
    {84, 1, 0, 0, 0, true, 0x00000000U, schema80802FB7},
}};
constexpr std::array<Field, 1> schema8080303A{{
    {0, 60, 4, 32, 4, false, 0x80000000U, {}},
}};
constexpr std::array<Field, 6> schema80803039{{
    {0, 1, 0, 32, 4, true, 0x80000000U, {}},
    {4, 1, 0, 1, 1, true, 0x00000000U, {}},
    {5, 1, 0, 3, 1, true, 0x00000001U, {}},
    {8, 1, 0, 32, 4, true, 0x80000000U, {}},
    {12, 1, 0, 1, 1, true, 0x00000000U, {}},
    {16, 1, 0, 0, 0, true, 0x00000000U, schema8080303A},
}};
constexpr std::array<Field, 1> schema80807913{{
    {0, 4, 2, 16, 2, false, 0x00000000U, {}},
}};
constexpr std::array<Field, 11> schema80807821{{
    {0, 1, 0, 64, 8, true, 0x00000000U, {}},
    {8, 1, 0, 64, 8, true, 0x00000000U, {}},
    {16, 1, 0, 64, 8, true, 0x00000000U, {}},
    {24, 1, 0, 2, 1, true, 0x00000001U, {}},
    {32, 1, 0, 64, 8, true, 0x00000000U, {}},
    {40, 1, 0, 2, 1, true, 0x00000001U, {}},
    {41, 1, 0, 6, 1, true, 0x00000001U, {}},
    {42, 1, 0, 5, 1, true, 0x00000000U, {}},
    {48, 1, 0, 64, 8, false, 0x00000000U, {}},
    {56, 1, 0, 3, 1, true, 0x00000001U, {}},
    {57, 1, 0, 1, 1, false, 0x00000000U, {}},
}};
constexpr std::array<Field, 1> schema80804459{{
    {0, 22, 4, 32, 4, false, 0x00000000U, {}},
}};
constexpr std::array<Field, 1> schema80802C62{{
    {0, 100, 2, 16, 2, true, 0x00008000U, {}},
}};
constexpr std::array<Field, 3> schema80802C63{{
    {0, 1, 0, 0, 0, false, 0x00000000U, schema80802C62},
    {200, 1, 0, 32, 4, false, 0x80000000U, {}},
    {204, 1, 0, 32, 4, false, 0x80000000U, {}},
}};
constexpr std::array<Field, 2> schema808042C1{{
    {0, 1, 0, 0, 0, true, 0x00000000U, schema80802C63},
    {208, 1, 0, 16, 2, true, 0x00008000U, {}},
}};
constexpr std::array<Field, 1> schema80802D7F{{
    {0, 30, 2, 16, 2, false, 0x00008000U, {}},
}};
constexpr std::array<Field, 3> schema80802E04{{
    {0, 1, 0, 0, 0, false, 0x00000000U, schema80802D7F},
    {60, 1, 0, 32, 4, false, 0x80000000U, {}},
    {64, 1, 0, 32, 4, false, 0x80000000U, {}},
}};
constexpr std::array<Field, 14> schema80807820{{
    {0, 1, 0, 0, 0, true, 0x00000000U, schema80807835},
    {16, 1, 0, 0, 0, true, 0x00000000U, schema80807827},
    {704, 1, 0, 0, 0, true, 0x00000000U, schema80803039},
    {960, 1, 0, 0, 0, true, 0x00000000U, schema80807913},
    {968, 1, 0, 0, 0, true, 0x00000000U, schema80807821},
    {1032, 1, 0, 0, 0, true, 0x00000000U, schema80804459},
    {1120, 1, 0, 0, 0, true, 0x00000000U, schema808042C1},
    {1332, 1, 0, 1, 1, true, 0x00000000U, {}},
    {1333, 1, 0, 1, 1, true, 0x00000000U, {}},
    {1334, 1, 0, 1, 1, true, 0x00000000U, {}},
    {1335, 1, 0, 8, 1, true, 0x00000080U, {}},
    {1336, 1, 0, 32, 4, true, 0x00000000U, {}},
    {1340, 1, 0, 0, 0, true, 0x00000000U, schema80802E04},
    {1408, 1, 0, 32, 4, true, 0x00000000U, {}},
}};
constexpr std::array<Field, 2> schema8080781F{{
    {0, 1, 0, 0, 0, true, 0x00000000U, schema80807837},
    {1080, 1, 0, 0, 0, true, 0x00000000U, schema80807820},
}};
constexpr std::array<Field, 1> schema80807603{{
    {0, 1, 0, 0, 0, false, 0x00000000U, schema8080781F},
}};
