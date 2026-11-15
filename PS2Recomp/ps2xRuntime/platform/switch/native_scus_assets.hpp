#pragma once
#include "native_assets.hpp"
#include <array>

namespace celosia::native {
// SCUS_974.72 archive directory metadata only. No game bytes are embedded.
inline constexpr std::array<AssetEntry,19> ScusAssets{{
    {"SYSTEM.CNF",2672949248ull,57ull},
    {"SCUS_974.72",2672674816ull,242940ull},
    {"GAMECORE.XFF",843776ull,2141864ull},
    {"IOPRP300.IMG",2985984ull,275345ull},
    {"KERNEL.XFF",3262464ull,415908ull},
    {"MANAGER.XFF",3680256ull,1650876ull},
    {"NICO.DAT",5332992ull,2667340678ull},
    {"SG2IOPM1.IRX",2672918528ull,27661ull},
    {"STARTUP.XFF",2672947200ull,26ull},
    {"MODULES2/MCMAN.IRX",595968ull,96181ull},
    {"MODULES2/MCSERV.IRX",692224ull,7385ull},
    {"MODULES/DBCMAN.IRX",700416ull,15653ull},
    {"MODULES/DS1O_D.IRX",716800ull,10469ull},
    {"MODULES/LIBSD.IRX",729088ull,30085ull},
    {"MODULES/MC2_D.IRX",759808ull,20269ull},
    {"MODULES/PL2303.IRX",780288ull,4269ull},
    {"MODULES/SIO2D.IRX",786432ull,11289ull},
    {"MODULES/SIO2MAN.IRX",798720ull,6641ull},
    {"MODULES/USBD.IRX",806912ull,34993ull},
}};
}
