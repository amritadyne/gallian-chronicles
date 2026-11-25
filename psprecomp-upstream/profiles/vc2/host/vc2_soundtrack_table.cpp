// VC2 soundtrack import table: which album track each game music track is,
// and where it lines up. Names, lengths and offsets only; no audio.
// Offsets are in samples at 44.1 kHz against the reference rip
// (album sample = game sample + lag); the import measures how the player's
// own rip differs and corrects for it. Made by imp/make_tables.py.
#include "vc2_soundtrack_table.hpp"

namespace vc2 {

using namespace portablekit::audio::soundtrack;

const std::vector<Entry> &soundtrack_table() {
    static const std::vector<Entry> table = [] {
        std::vector<Entry> t;
        const std::string album = "Valkyria Chronicles 2 Original Soundtrack";
        { // STR_01: 1-02 Exciting Battle
            Entry e{"STR_01", "9f541332b8aed991", "1-02", "Exciting Battle", album, 189.15, 21871, Method::Mix, 21822, {}};
            t.push_back(std::move(e));
        }
        { // STR_02: 1-09 Liberation Battle
            Entry e{"STR_02", "01aaf3ec2bef94f0", "1-09", "Liberation Battle", album, 160.16, 22075, Method::Exact, 22049, {}};
            t.push_back(std::move(e));
        }
        { // STR_03: 1-15 Outside Battle
            Entry e{"STR_03", "88c9f366bd01331c", "1-15", "Outside Battle", album, 93.65, 22003, Method::Exact, 21869, {}};
            t.push_back(std::move(e));
        }
        { // STR_04: 1-17 Snowfield Battle
            Entry e{"STR_04", "c94c95f90aed1535", "1-17", "Snowfield Battle", album, 214.47, 22014, Method::Exact, 22005, {}};
            t.push_back(std::move(e));
        }
        { // STR_05: 1-20 Street Battle
            Entry e{"STR_05", "21fa6287e2909148", "1-20", "Street Battle", album, 80.2, 21813, Method::Exact, 21543, {}};
            t.push_back(std::move(e));
        }
        { // STR_06: 1-23 Desert Battle
            Entry e{"STR_06", "55a45266f33afb07", "1-23", "Desert Battle", album, 169.09, 21995, Method::Exact, 22041, {}};
            t.push_back(std::move(e));
        }
        { // STR_07: 2-07 Close Battle
            Entry e{"STR_07", "66b9893596bc7cf4", "2-07", "Close Battle", album, 104.33, 21867, Method::Exact, 21825, {}};
            t.push_back(std::move(e));
        }
        { // STR_08: 2-09 Mine Battle
            Entry e{"STR_08", "5dd4e9bc459241e4", "2-09", "Mine Battle", album, 189.91, 22082, Method::Exact, 3463326, {}};
            t.push_back(std::move(e));
        }
        { // STR_09: 2-11 Offensive and Defensive Battle
            Entry e{"STR_09", "90a848b7e725f464", "2-11", "Offensive and Defensive Battle", album, 85.73, 22070, Method::Exact, 21789, {}};
            t.push_back(std::move(e));
        }
        { // STR_10: 2-14 Ship Battle
            Entry e{"STR_10", "21dfe7cfa1a5a09d", "2-14", "Ship Battle", album, 254.07, 21723, Method::Mix, 21100, {}};
            t.push_back(std::move(e));
        }
        { // STR_11: 1-10 Counter Battle
            Entry e{"STR_11", "bf0ab466908a91e4", "1-10", "Counter Battle", album, 169.77, 22244, Method::Mix, 22427, {}};
            t.push_back(std::move(e));
        }
        { // STR_12: 1-21 Defense Battle
            Entry e{"STR_12", "5757028ab8f878ee", "1-21", "Defense Battle", album, 121.52, 21902, Method::Exact, 21805, {}};
            t.push_back(std::move(e));
        }
        { // STR_13: 1-24 Decisive Battle
            Entry e{"STR_13", "5d2c26a26564bcd4", "1-24", "Decisive Battle", album, 180.82, 21898, Method::Exact, 21884, {}};
            t.push_back(std::move(e));
        }
        { // STR_14: 2-16 Final Decisive Battle
            Entry e{"STR_14", "3db346ea82255df4", "2-16", "Final Decisive Battle", album, 278.6, 21994, Method::Exact, 22053, {}};
            t.push_back(std::move(e));
        }
        { // STR_15: 1-25 Defeat
            Entry e{"STR_15", "91c931f5f4c02588", "1-25", "Defeat", album, 17.92, 21933, Method::Exact, 21880, {}};
            t.push_back(std::move(e));
        }
        { // STR_16: 1-11 Reward
            Entry e{"STR_16", "9e50cb0841ed6808", "1-11", "Reward", album, 79.73, 21782, Method::Exact, 21604, {}};
            t.push_back(std::move(e));
        }
        { // STR_17: 1-13 Spring in Lanseal
            Entry e{"STR_17", "b9d72fccb8544085", "1-13", "Spring in Lanseal", album, 131.11, 21672, Method::Exact, 21650, {}};
            t.push_back(std::move(e));
        }
        { // STR_18: 1-19 Summer in Lanseal
            Entry e{"STR_18", "0934c4d311c44031", "1-19", "Summer in Lanseal", album, 167.5, 21727, Method::Exact, 3047276, {}};
            t.push_back(std::move(e));
        }
        { // STR_19: 2-06 Autumn in Lanseal
            Entry e{"STR_19", "4f3f1aba959c7556", "2-06", "Autumn in Lanseal", album, 170.76, 21754, Method::Exact, 3197746, {}};
            t.push_back(std::move(e));
        }
        { // STR_20: 1-03 Winter in Lanseal
            Entry e{"STR_20", "817316954891004b", "1-03", "Winter in Lanseal", album, 145.5, 21734, Method::Exact, 21746, {}};
            t.push_back(std::move(e));
        }
        { // STR_21: 1-08 Briefing
            Entry e{"STR_21", "29e68202c43ca4bb", "1-08", "Briefing", album, 168.28, 22311, Method::Exact, 22441, {}};
            t.push_back(std::move(e));
        }
        { // STR_22: 1-18 Theme of The Rebels
            Entry e{"STR_22", "aea7f13431776626", "1-18", "Theme of The Rebels", album, 197.46, 21680, Method::Exact, 21972, {}};
            t.push_back(std::move(e));
        }
        { // STR_23: 2-12 Theme of Valkyria
            Entry e{"STR_23", "93a9032c63f6d45a", "2-12", "Theme of Valkyria", album, 180.74, 21691, Method::Mix, 21891, {}};
            t.push_back(std::move(e));
        }
        { // STR_24: 1-04 Everyday Life of The Class
            Entry e{"STR_24", "ea471f3453bc467f", "1-04", "Everyday Life of The Class", album, 158.85, 22037, Method::Exact, 22141, {}};
            t.push_back(std::move(e));
        }
        { // STR_25: 2-02 Days With Classmates
            Entry e{"STR_25", "c72d99aca447ec41", "2-02", "Days With Classmates", album, 149.21, 21809, Method::Exact, 22269, {}};
            t.push_back(std::move(e));
        }
        { // STR_26: 1-22 Conflict
            Entry e{"STR_26", "8a68a9edcf60ad97", "1-22", "Conflict", album, 163.48, 21920, Method::Exact, 21927, {}};
            t.push_back(std::move(e));
        }
        { // STR_27: 2-08 Tear at The War
            Entry e{"STR_27", "1d8be529b1a00c58", "2-08", "Tear at The War", album, 183.33, 23418, Method::Exact, 3921689, {}};
            t.push_back(std::move(e));
        }
        { // STR_28: 2-10 Faint Youth
            Entry e{"STR_28", "7b63db69362ac89b", "2-10", "Faint Youth", album, 126.25, 22361, Method::Exact, 23181, {}};
            t.push_back(std::move(e));
        }
        { // STR_29: 1-16 War Preparedness
            Entry e{"STR_29", "a98bcd25166968cd", "1-16", "War Preparedness", album, 106.92, 22061, Method::Exact, 22140, {}};
            t.push_back(std::move(e));
        }
        { // STR_30: 2-04 Theme of Class G
            Entry e{"STR_30", "72deaef67d6e3b02", "2-04", "Theme of Class G", album, 161.62, 22027, Method::Exact, 22171, {}};
            t.push_back(std::move(e));
        }
        { // STR_31: 1-14 Are We A Spoiled Class
            Entry e{"STR_31", "ed3c0d6875a72f0a", "1-14", "Are We A Spoiled Class", album, 120.22, 22091, Method::Exact, 22144, {}};
            t.push_back(std::move(e));
        }
        { // STR_32: 2-15 Gassenarl's Ambition
            Entry e{"STR_32", "82c4d84b9aef0dbc", "2-15", "Gassenarl's Ambition", album, 150.13, 22421, Method::Exact, 3204733, {}};
            t.push_back(std::move(e));
        }
        { // STR_33: 1-06 Royal Military Academy
            Entry e{"STR_33", "57d6e3e5b85b1bf5", "1-06", "Royal Military Academy", album, 139.86, 22555, Method::Exact, 22587, {}};
            t.push_back(std::move(e));
        }
        { // STR_34: 2-17 Graduation
            Entry e{"STR_34", "7ed796875cf46714", "2-17", "Graduation", album, 102.66, 21900, Method::Recipe, 0, {}};
            // The album opening, a bridge heard only in the game (its own audio), then the
            // album main part, which the game plays 2.24% faster (pitch kept).
            e.recipe.sections.push_back({Section::Source::Album, 0, 604614, 26016, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::Game, 604614, 904494, 0, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::AlbumStretched, 904494, 3442818, 0, 7.6939, 1.02241, 0});
            e.recipe.crossfade = 2205;
            e.recipe.level_each_section = true;
            e.recipe.loose = true; // the game's Graduation is a different render of the album recording
            t.push_back(std::move(e));
        }
        { // STR_35: 1-05 Practice Battle
            Entry e{"STR_35", "d1519f4026fe735b", "1-05", "Practice Battle", album, 184.97, 22310, Method::Exact, 22231, {}};
            t.push_back(std::move(e));
        }
        { // STR_37: 2-03 Laevatein Cup Finals
            Entry e{"STR_37", "39cc9094aa685ffb", "2-03", "Laevatein Cup Finals", album, 280.63, 21889, Method::Exact, 21952, {}};
            t.push_back(std::move(e));
        }
        { // STR_38: 1-01 Valkyria Chronicles 2 Main Theme
            Entry e{"STR_38", "ba8be2349b1e56bf", "1-01", "Valkyria Chronicles 2 Main Theme", album, 212.32, 32296, Method::Recipe, 0, {}};
            // The game's title music is six album sections cut together; the game's
            // own audio is used for 0.4 s each side of every join (its mix was made for the edit).
            e.recipe.sections.push_back({Section::Source::Album, 0, 588470, 35174, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::Album, 588470, 904005, 642971, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::Album, 904005, 1213808, 1249144, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::Album, 1213808, 1733085, 1867243, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::Album, 1733085, 2278426, 6081937, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::Album, 2278426, 2800350, 6412583, 0.0, 1.0, 0});
            e.recipe.crossfade = 441;
            e.recipe.equal_power = true;
            e.recipe.join_half = 17640;
            e.recipe.join_fade = 4410;
            t.push_back(std::move(e));
        }
        return t;
    }();
    return table;
}

} // namespace vc2
