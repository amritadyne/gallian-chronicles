// VC3 soundtrack import table: which album track each game music track is,
// and where it lines up. Names, lengths and offsets only; no audio. VC3 uses
// tracks from three soundtracks (VC3, VC2 and VC1). Offsets are in samples at
// 44.1 kHz against the reference rip (album sample = game sample + lag); the
// import measures how the player's own rip differs and corrects for it.
// Tracks are known by key; `track` is the offset in the decrypted DATA.BIN.
// VC3's music can't be read from the disc without the running game (its data
// is decrypted with a key the game hands over), so every entry carries the game
// track's length and loudness and is built from the album alone.
// Left out: the 117 s track at 323995680 (a 2-01 opening, then music on no album).
#include "vc3_soundtrack_table.hpp"

namespace vc3 {

using namespace portablekit::audio::soundtrack;

const std::vector<Entry> &soundtrack_table() {
    static const std::vector<Entry> table = [] {
        std::vector<Entry> t;
        { // VC3 2-01 Valkyria Chronicles 3 Main Theme (Short Ver.)
            Entry e{"0279062560", "d7bf609ae1d43646", "2-01", "Valkyria Chronicles 3 Main Theme (Short Ver.)", "Valkyria Chronicles 3 Original Soundtrack", 73.27, 21403, Method::Exact, 22811, {}};
            e.game_frames = 2964928;
            e.game_rms = 9385.06;
            t.push_back(std::move(e));
        }
        { // VC3 1-02 Fierce Battlefield
            Entry e{"0279611424", "612495008ae91c9c", "1-02", "Fierce Battlefield", "Valkyria Chronicles 3 Original Soundtrack", 191.37, 20919, Method::Exact, 20927, {}};
            e.game_frames = 3538031;
            e.game_rms = 8009.4;
            t.push_back(std::move(e));
        }
        { // VC3 1-04 Counter Offence!
            Entry e{"0280264736", "c83029605f0712f6", "1-04", "Counter Offence!", "Valkyria Chronicles 3 Original Soundtrack", 205.74, 20463, Method::Exact, 20403, {}};
            e.game_frames = 3916084;
            e.game_rms = 7591.91;
            t.push_back(std::move(e));
        }
        { // VC2 1-23 Desert Battle
            Entry e{"0280987680", "55a45266f33afb07", "1-23", "Desert Battle", "Valkyria Chronicles 2 Original Soundtrack", 169.09, 21995, Method::Exact, 22041, {}};
            e.game_frames = 3666474;
            e.game_rms = 8154.34;
            t.push_back(std::move(e));
        }
        { // VC2 1-09 Liberation Battle
            Entry e{"0281665568", "01aaf3ec2bef94f0", "1-09", "Liberation Battle", "Valkyria Chronicles 2 Original Soundtrack", 160.16, 22075, Method::Exact, 22049, {}};
            e.game_frames = 2930300;
            e.game_rms = 8294.1;
            t.push_back(std::move(e));
        }
        { // VC2 1-17 Snowfield Battle
            Entry e{"0282208288", "c94c95f90aed1535", "1-17", "Snowfield Battle", "Valkyria Chronicles 2 Original Soundtrack", 214.47, 22014, Method::Exact, 22005, {}};
            e.game_frames = 4118953;
            e.game_rms = 8408.09;
            t.push_back(std::move(e));
        }
        { // VC2 2-11 Offensive and Defensive Battle
            Entry e{"0282968096", "90a848b7e725f464", "2-11", "Offensive and Defensive Battle", "Valkyria Chronicles 2 Original Soundtrack", 85.73, 22070, Method::Exact, 21789, {}};
            e.game_frames = 3603571;
            e.game_rms = 7684.26;
            t.push_back(std::move(e));
        }
        { // VC2 1-15 Outside Battle
            Entry e{"0283633696", "88c9f366bd01331c", "1-15", "Outside Battle", "Valkyria Chronicles 2 Original Soundtrack", 93.65, 22003, Method::Exact, 21869, {}};
            e.game_frames = 3807047;
            e.game_rms = 7358.73;
            t.push_back(std::move(e));
        }
        { // VC3 2-08 Must-win Battle
            Entry e{"0284336160", "4000a4b6bb2f75d6", "2-08", "Must-win Battle", "Valkyria Chronicles 3 Original Soundtrack", 186.88, 20492, Method::Exact, 3405324, {}};
            e.game_frames = 3526051;
            e.game_rms = 10399.9;
            t.push_back(std::move(e));
        }
        { // VC2 2-07 Close Battle
            Entry e{"0284987424", "66b9893596bc7cf4", "2-07", "Close Battle", "Valkyria Chronicles 2 Original Soundtrack", 104.33, 21867, Method::Exact, 21825, {}};
            e.game_frames = 4230740;
            e.game_rms = 7277.62;
            t.push_back(std::move(e));
        }
        { // VC2 1-05 Practice Battle
            Entry e{"0285767712", "d1519f4026fe735b", "1-05", "Practice Battle", "Valkyria Chronicles 2 Original Soundtrack", 184.97, 22310, Method::Exact, 22231, {}};
            e.game_frames = 3664254;
            e.game_rms = 7534.91;
            t.push_back(std::move(e));
        }
        { // VC1 1-04 Gallant Fight
            Entry e{"0286443552", "4459af71ea9f65d7", "1-04", "Gallant Fight", "Valkyria Chronicles Original Soundtrack", 210.76, 18972, Method::Exact, 18996, {}};
            e.game_frames = 4482385;
            e.game_rms = 7943.52;
            t.push_back(std::move(e));
        }
        { // VC3 1-05 The Looming Sound of Ammunition Boots
            Entry e{"0287270944", "7d19c9d9eb2eb579", "1-05", "The Looming Sound of Ammunition Boots", "Valkyria Chronicles 3 Original Soundtrack", 151.54, 20841, Method::Exact, 20740, {}};
            e.game_frames = 2648308;
            e.game_rms = 7759.85;
            t.push_back(std::move(e));
        }
        { // VC1 1-14 Hard Fight
            Entry e{"0287758368", "67f908645d7fd62a", "1-14", "Hard Fight", "Valkyria Chronicles Original Soundtrack", 145.22, 19193, Method::Exact, 2941271, {}};
            e.game_frames = 3032208;
            e.game_rms = 7415.46;
            t.push_back(std::move(e));
        }
        { // VC3 1-17 Dark Hero
            Entry e{"0288319520", "e80a436a55aff6e8", "1-17", "Dark Hero", "Valkyria Chronicles 3 Original Soundtrack", 160.97, 20790, Method::Exact, 20742, {}};
            e.game_frames = 3000127;
            e.game_rms = 9561.27;
            t.push_back(std::move(e));
        }
        { // VC2 2-14 Ship Battle
            Entry e{"0288874528", "21dfe7cfa1a5a09d", "2-14", "Ship Battle", "Valkyria Chronicles 2 Original Soundtrack", 254.07, 21723, Method::Mix, 21100, {}};
            e.game_frames = 5276155;
            e.game_rms = 7498.21;
            t.push_back(std::move(e));
        }
        { // VC3 2-11 Final Decisive Battle
            Entry e{"0289847328", "b13393e9dbb9643c", "2-11", "Final Decisive Battle", "Valkyria Chronicles 3 Original Soundtrack", 264.05, 20548, Method::Exact, 20546, {}};
            e.game_frames = 5164477;
            e.game_rms = 10494.83;
            t.push_back(std::move(e));
        }
        { // VC3 1-12 Calamity Raven
            Entry e{"0290799648", "71503733d71b4719", "1-12", "Calamity Raven", "Valkyria Chronicles 3 Original Soundtrack", 143.57, 21173, Method::Exact, 20798, {}};
            e.game_frames = 2535242;
            e.game_rms = 10450.88;
            t.push_back(std::move(e));
        }
        { // VC2 1-11 Reward
            Entry e{"0291266592", "9e50cb0841ed6808", "1-11", "Reward", "Valkyria Chronicles 2 Original Soundtrack", 79.73, 21782, Method::Exact, 21604, {}};
            e.game_frames = 1829163;
            e.game_rms = 8322.32;
            t.push_back(std::move(e));
        }
        { // VC2 1-25 Defeat
            Entry e{"0291604512", "91c931f5f4c02588", "1-25", "Defeat", "Valkyria Chronicles 2 Original Soundtrack", 17.92, 21933, Method::Exact, 21880, {}};
            e.game_frames = 679392;
            e.game_rms = 6620.75;
            t.push_back(std::move(e));
        }
        { // VC3 1-03 In Search Of The Next Battle
            Entry e{"0291731488", "485f14007cc8b9b7", "1-03", "In Search Of The Next Battle", "Valkyria Chronicles 3 Original Soundtrack", 189.28, 21161, Method::Exact, 21072, {}};
            e.game_frames = 3555195;
            e.game_rms = 7435.56;
            t.push_back(std::move(e));
        }
        { // VC1 1-13 Urgent Instructions
            Entry e{"0292388896", "7f6ba7752a4aa58b", "1-13", "Urgent Instructions", "Valkyria Chronicles Original Soundtrack", 108.28, 18986, Method::Exact, 2132732, {}};
            e.game_frames = 2196365;
            e.game_rms = 7465.19;
            t.push_back(std::move(e));
        }
        { // VC1 1-12 Strategy Instructions
            Entry e{"0292794400", "ce9ebccaa5787c75", "1-12", "Strategy Instructions", "Valkyria Chronicles Original Soundtrack", 163.27, 19301, Method::Exact, 19268, {}};
            e.game_frames = 3406124;
            e.game_rms = 7433.44;
            t.push_back(std::move(e));
        }
        { // VC3 1-14 A Momentary Rest
            Entry e{"0293423136", "d3172ed18431a497", "1-14", "A Momentary Rest", "Valkyria Chronicles 3 Original Soundtrack", 180.48, 22235, Method::Exact, 20964, {}};
            e.game_frames = 3383722;
            e.game_rms = 10408.8;
            t.push_back(std::move(e));
        }
        { // VC3 1-16 Are the Customers Pigeons
            Entry e{"0294047776", "b300b254d5df9691", "1-16", "Are the Customers Pigeons", "Valkyria Chronicles 3 Original Soundtrack", 162.04, 20762, Method::Exact, 20647, {}};
            e.game_frames = 3059125;
            e.game_rms = 8031.37;
            t.push_back(std::move(e));
        }
        { // VC1 1-06 Everyday Training
            Entry e{"0294613024", "12598e8d6144f8ae", "1-06", "Everyday Training", "Valkyria Chronicles Original Soundtrack", 109.32, 19090, Method::Exact, 18917, {}};
            e.game_frames = 2193004;
            e.game_rms = 7776.19;
            t.push_back(std::move(e));
        }
        { // VC3 2-04 'The Nameless', Takeoff!
            Entry e{"0295016480", "e274b8f73496388c", "2-04", "'The Nameless', Takeoff!", "Valkyria Chronicles 3 Original Soundtrack", 182.65, 20875, Method::Exact, 20850, {}};
            e.game_frames = 3440941;
            e.game_rms = 10139.94;
            t.push_back(std::move(e));
        }
        { // VC3 1-09 Swirling Conspiracy
            Entry e{"0295651360", "233c9e2e2a2a3f86", "1-09", "Swirling Conspiracy", "Valkyria Chronicles 3 Original Soundtrack", 160.6, 22003, Method::Exact, 21131, {}};
            e.game_frames = 2925471;
            e.game_rms = 9933.97;
            t.push_back(std::move(e));
        }
        { // VC2 2-12 Theme of Valkyria
            Entry e{"0296192032", "93a9032c63f6d45a", "2-12", "Theme of Valkyria", "Valkyria Chronicles 2 Original Soundtrack", 180.74, 21691, Method::Mix, 21891, {}};
            e.game_frames = 3383297;
            e.game_rms = 6930.31;
            t.push_back(std::move(e));
        }
        { // VC3 1-07 Daily Lives Of 'The Nameless'
            Entry e{"0296816672", "173459486122916c", "1-07", "Daily Lives Of 'The Nameless'", "Valkyria Chronicles 3 Original Soundtrack", 182.28, 21344, Method::Exact, 21206, {}};
            e.game_frames = 3271809;
            e.game_rms = 7088.6;
            t.push_back(std::move(e));
        }
        { // VC3 1-06 We Are 'The Nameless'
            Entry e{"0297420832", "26c68b7762293a92", "1-06", "We Are 'The Nameless'", "Valkyria Chronicles 3 Original Soundtrack", 179.88, 20817, Method::Exact, 20803, {}};
            e.game_frames = 3308245;
            e.game_rms = 8678.61;
            t.push_back(std::move(e));
        }
        { // VC3 1-08 Signs Of Unrest
            Entry e{"0298031136", "ab67c00d710e3138", "1-08", "Signs Of Unrest", "Valkyria Chronicles 3 Original Soundtrack", 139.57, 21301, Method::Exact, 21068, {}};
            e.game_frames = 2406798;
            e.game_rms = 8563.74;
            t.push_back(std::move(e));
        }
        { // VC3 1-18 That Is 'The Nameless'
            Entry e{"0298475552", "f6460f0cedfb9ebd", "1-18", "That Is 'The Nameless'", "Valkyria Chronicles 3 Original Soundtrack", 203.02, 21150, Method::Exact, 20525, {}};
            e.game_frames = 3939768;
            e.game_rms = 7728.54;
            t.push_back(std::move(e));
        }
        { // VC3 1-19 Connecting Hearts
            Entry e{"0299202592", "3bec22c268fbc2c5", "1-19", "Connecting Hearts", "Valkyria Chronicles 3 Original Soundtrack", 174.11, 21321, Method::Exact, 21197, {}};
            e.game_frames = 3142438;
            e.game_rms = 8077.68;
            t.push_back(std::move(e));
        }
        { // VC3 1-13 Critical Moment!
            Entry e{"0299784224", "ebf22081a95c00b8", "1-13", "Critical Moment!", "Valkyria Chronicles 3 Original Soundtrack", 123.53, 20880, Method::Exact, 20633, {}};
            e.game_frames = 2104819;
            e.game_rms = 8508.15;
            t.push_back(std::move(e));
        }
        { // VC3 2-03 As A Squad
            Entry e{"0300173344", "01cdbb7445f49015", "2-03", "As A Squad", "Valkyria Chronicles 3 Original Soundtrack", 204.93, 20385, Method::Recipe, 19675, {}};
            e.game_frames = 3881421;
            e.game_rms = 6776.41;
            // The game's version has an edited ending: the album at a second offset from 84.9 s.
            e.recipe.sections.push_back({Section::Source::Album, 0, 3744090, 19675, 0.0, 1.0, 0});
            e.recipe.sections.push_back({Section::Source::Album, 3744090, 3881421, 22395, 0.0, 1.0, 0});
            e.recipe.crossfade = 441;
            e.recipe.equal_power = true;
            t.push_back(std::move(e));
        }
        { // VC3 2-09 With You
            Entry e{"0300890144", "7144e57a0e8a359b", "2-09", "With You", "Valkyria Chronicles 3 Original Soundtrack", 159.24, 21158, Method::Exact, 21096, {}};
            e.game_frames = 2861282;
            e.game_rms = 7099.46;
            t.push_back(std::move(e));
        }
        { // VC3 2-10 Steady Preparedness
            Entry e{"0301418528", "1b27c3abd5a39bb7", "2-10", "Steady Preparedness", "Valkyria Chronicles 3 Original Soundtrack", 171.57, 21001, Method::Exact, 20674, {}};
            e.game_frames = 3174281;
            e.game_rms = 7525.6;
            t.push_back(std::move(e));
        }
        { // VC3 2-02 Riela Marceris
            Entry e{"0302006304", "5ac98e54b816abfa", "2-02", "Riela Marceris", "Valkyria Chronicles 3 Original Soundtrack", 227.27, 21335, Method::Exact, 4256207, {}};
            e.game_frames = 4308562;
            e.game_rms = 6845.0;
            t.push_back(std::move(e));
        }
        { // VC3 1-15 Damn It!
            Entry e{"0302800928", "09b816e4e31d014b", "1-15", "Damn It!", "Valkyria Chronicles 3 Original Soundtrack", 198.66, 21436, Method::Exact, 21302, {}};
            e.game_frames = 4264260;
            e.game_rms = 9746.66;
            t.push_back(std::move(e));
        }
        { // VC3 1-10 Fear And Guts
            Entry e{"0303587360", "6272f307e0221cb1", "1-10", "Fear And Guts", "Valkyria Chronicles 3 Original Soundtrack", 199.16, 21076, Method::Mix, 20897, {}};
            e.game_frames = 3670434;
            e.game_rms = 9139.48;
            t.push_back(std::move(e));
        }
        { // VC3 2-06 OPEN FIRE!
            Entry e{"0304265248", "9bfa564855a0b573", "2-06", "OPEN FIRE!", "Valkyria Chronicles 3 Original Soundtrack", 159.97, 20900, Method::Exact, 20693, {}};
            e.game_frames = 2903883;
            e.game_rms = 10118.48;
            t.push_back(std::move(e));
        }
        { // VC2 2-03 Laevatein Cup Finals
            Entry e{"0304801824", "39cc9094aa685ffb", "2-03", "Laevatein Cup Finals", "Valkyria Chronicles 2 Original Soundtrack", 280.63, 21889, Method::Exact, 21952, {}};
            e.game_frames = 5533665;
            e.game_rms = 8626.04;
            t.push_back(std::move(e));
        }
        { // VC2 1-18 Theme of The Rebels
            Entry e{"0305821728", "aea7f13431776626", "1-18", "Theme of The Rebels", "Valkyria Chronicles 2 Original Soundtrack", 197.46, 21680, Method::Exact, 21972, {}};
            e.game_frames = 3720378;
            e.game_rms = 8121.18;
            t.push_back(std::move(e));
        }
        { // VC2 1-04 Everyday Life of The Class
            Entry e{"0306507808", "ea471f3453bc467f", "1-04", "Everyday Life of The Class", "Valkyria Chronicles 2 Original Soundtrack", 158.85, 22037, Method::Exact, 22141, {}};
            e.game_frames = 2830374;
            e.game_rms = 7785.21;
            t.push_back(std::move(e));
        }
        { // VC2 2-02 Days With Classmates
            Entry e{"0307032096", "c72d99aca447ec41", "2-02", "Days With Classmates", "Valkyria Chronicles 2 Original Soundtrack", 149.21, 21809, Method::Exact, 22269, {}};
            e.game_frames = 2634607;
            e.game_rms = 7198.28;
            t.push_back(std::move(e));
        }
        { // VC2 1-22 Conflict
            Entry e{"0307517472", "8a68a9edcf60ad97", "1-22", "Conflict", "Valkyria Chronicles 2 Original Soundtrack", 163.48, 21920, Method::Exact, 21927, {}};
            e.game_frames = 2926172;
            e.game_rms = 8031.02;
            t.push_back(std::move(e));
        }
        { // VC2 2-08 Tear at The War
            Entry e{"0308058144", "1d8be529b1a00c58", "2-08", "Tear at The War", "Valkyria Chronicles 2 Original Soundtrack", 183.33, 23418, Method::Exact, 3921689, {}};
            e.game_frames = 3994418;
            e.game_rms = 5472.23;
            t.push_back(std::move(e));
        }
        { // VC2 2-10 Faint Youth
            Entry e{"0308795424", "7b63db69362ac89b", "2-10", "Faint Youth", "Valkyria Chronicles 2 Original Soundtrack", 126.25, 22361, Method::Exact, 23181, {}};
            e.game_frames = 2744321;
            e.game_rms = 7294.24;
            t.push_back(std::move(e));
        }
        { // VC2 1-16 War Preparedness
            Entry e{"0309301280", "a98bcd25166968cd", "1-16", "War Preparedness", "Valkyria Chronicles 2 Original Soundtrack", 106.92, 22061, Method::Exact, 22140, {}};
            e.game_frames = 1772873;
            e.game_rms = 8992.73;
            t.push_back(std::move(e));
        }
        { // VC2 2-04 Theme of Class G
            Entry e{"0309628960", "72deaef67d6e3b02", "2-04", "Theme of Class G", "Valkyria Chronicles 2 Original Soundtrack", 161.62, 22027, Method::Exact, 22171, {}};
            e.game_frames = 2930320;
            e.game_rms = 7241.61;
            t.push_back(std::move(e));
        }
        { // VC2 1-14 Are We A Spoiled Class
            Entry e{"0310171680", "ed3c0d6875a72f0a", "1-14", "Are We A Spoiled Class", "Valkyria Chronicles 2 Original Soundtrack", 120.22, 22091, Method::Exact, 22144, {}};
            e.game_frames = 2566020;
            e.game_rms = 5855.84;
            t.push_back(std::move(e));
        }
        { // VC2 1-06 Royal Military Academy
            Entry e{"0310644768", "57d6e3e5b85b1bf5", "1-06", "Royal Military Academy", "Valkyria Chronicles 2 Original Soundtrack", 139.86, 22555, Method::Exact, 22587, {}};
            e.game_frames = 2487124;
            e.game_rms = 8915.54;
            t.push_back(std::move(e));
        }
        { // VC2 1-08 Briefing
            Entry e{"0311103520", "29e68202c43ca4bb", "1-08", "Briefing", "Valkyria Chronicles 2 Original Soundtrack", 168.28, 22311, Method::Exact, 22441, {}};
            e.game_frames = 3152471;
            e.game_rms = 8382.87;
            t.push_back(std::move(e));
        }
        { // VC1 2-06 7th Platoon of the Volunteer Army
            Entry e{"0311687200", "6bada931942dcae4", "2-06", "7th Platoon of the Volunteer Army", "Valkyria Chronicles Original Soundtrack", 123.56, 19327, Method::Exact, 19241, {}};
            e.game_frames = 2532427;
            e.game_rms = 7727.5;
            t.push_back(std::move(e));
        }
        { // VC1 1-05 Randgrith City
            Entry e{"0312154144", "1ecb2be688395872", "1-05", "Randgrith City", "Valkyria Chronicles Original Soundtrack", 116.72, 19017, Method::Exact, 18994, {}};
            e.game_frames = 2266978;
            e.game_rms = 8893.79;
            t.push_back(std::move(e));
        }
        { // VC1 1-17 The Gallian War
            Entry e{"0312571936", "bf2ce80ddb0c9be9", "1-17", "The Gallian War", "Valkyria Chronicles Original Soundtrack", 177.69, 19133, Method::Exact, 19013, {}};
            e.game_frames = 3700951;
            e.game_rms = 8515.87;
            t.push_back(std::move(e));
        }
        { // VC1 1-20 War in the Empire
            Entry e{"0313255968", "0c664745e0228d41", "1-20", "War in the Empire", "Valkyria Chronicles Original Soundtrack", 204.93, 19134, Method::Exact, 18830, {}};
            e.game_frames = 4551308;
            e.game_rms = 7674.72;
            t.push_back(std::move(e));
        }
        { // VC1 1-24 Valkyria's Omen
            Entry e{"0314095648", "2cbd3cf3e0643433", "1-24", "Valkyria's Omen", "Valkyria Chronicles Original Soundtrack", 234.92, 19041, Method::Exact, 19054, {}};
            e.game_frames = 5156438;
            e.game_rms = 7049.45;
            t.push_back(std::move(e));
        }
        { // VC1 2-05 People of Darcus
            Entry e{"0315045920", "03fc12eb93294d8e", "2-05", "People of Darcus", "Valkyria Chronicles Original Soundtrack", 168.88, 18952, Method::Exact, 18832, {}};
            e.game_frames = 3506451;
            e.game_rms = 7564.04;
            t.push_back(std::move(e));
        }
        { // VC1 1-08 Hope for Tomorrow
            Entry e{"0315693088", "78e0b3d371a87e75", "1-08", "Hope for Tomorrow", "Valkyria Chronicles Original Soundtrack", 225.67, 19094, Method::Exact, 4723304, {}};
            e.game_frames = 4813415;
            e.game_rms = 9037.64;
            t.push_back(std::move(e));
        }
        { // VC1 2-04 A Moment of Relief
            Entry e{"0316579872", "31f588788519418c", "2-04", "A Moment of Relief", "Valkyria Chronicles Original Soundtrack", 206.63, 19525, Method::Exact, 19512, {}};
            e.game_frames = 4361377;
            e.game_rms = 7825.14;
            t.push_back(std::move(e));
        }
        { // VC1 2-14 Composition of the Confrontation
            Entry e{"0317384736", "e284482028338eea", "2-14", "Composition of the Confrontation", "Valkyria Chronicles Original Soundtrack", 205.51, 18949, Method::Exact, 18920, {}};
            e.game_frames = 4341641;
            e.game_rms = 7839.48;
            t.push_back(std::move(e));
        }
        { // VC1 2-01 Empty Loneliness
            Entry e{"0318185504", "63356bd45e2f6c8b", "2-01", "Empty Loneliness", "Valkyria Chronicles Original Soundtrack", 147.67, 18814, Method::Exact, 18784, {}};
            e.game_frames = 3018502;
            e.game_rms = 7186.82;
            t.push_back(std::move(e));
        }
        { // VC1 2-09 Anxiety
            Entry e{"0318744608", "aada08c1886aac1f", "2-09", "Anxiety", "Valkyria Chronicles Original Soundtrack", 214.73, 19167, Method::Exact, 19121, {}};
            e.game_frames = 4541274;
            e.game_rms = 7837.81;
            t.push_back(std::move(e));
        }
        { // VC1 2-16 Quiet Chat
            Entry e{"0319582240", "07b58a68e5d0959d", "2-16", "Quiet Chat", "Valkyria Chronicles Original Soundtrack", 159.14, 19024, Method::Exact, 18979, {}};
            e.game_frames = 3269715;
            e.game_rms = 7601.22;
            t.push_back(std::move(e));
        }
        { // VC1 2-18 Chris Draws Near
            Entry e{"0320186400", "7bd4a4cff97d6227", "2-18", "Chris Draws Near", "Valkyria Chronicles Original Soundtrack", 135.32, 18904, Method::Exact, 18843, {}};
            e.game_frames = 2765473;
            e.game_rms = 7590.38;
            t.push_back(std::move(e));
        }
        { // VC1 2-12 The Limits of the Quarrel
            Entry e{"0320696352", "f99eff37db7b1fa3", "2-12", "The Limits of the Quarrel", "Valkyria Chronicles Original Soundtrack", 188.32, 19260, Method::Exact, 3888779, {}};
            e.game_frames = 3869711;
            e.game_rms = 7917.05;
            t.push_back(std::move(e));
        }
        { // VC1 2-11 Recollection of That Day
            Entry e{"0321411104", "602d1650542ffaba", "2-11", "Recollection of That Day", "Valkyria Chronicles Original Soundtrack", 183.46, 19242, Method::Exact, 3787291, {}};
            e.game_frames = 3885676;
            e.game_rms = 7600.62;
            t.push_back(std::move(e));
        }
        { // VC3 2-07 Risking Everything On This Stroke
            Entry e{"0322127904", "fd27a0c7efa9b92a", "2-07", "Risking Everything On This Stroke", "Valkyria Chronicles 3 Original Soundtrack", 130.04, 20675, Method::Exact, 20759, {}};
            e.game_frames = 2330711;
            e.game_rms = 10509.8;
            t.push_back(std::move(e));
        }
        { // VC3 2-05 With Pride In Hearts
            Entry e{"0322557984", "cdb8b0ce218ddb63", "2-05", "With Pride In Hearts", "Valkyria Chronicles 3 Original Soundtrack", 238.13, 20683, Method::Exact, 20594, {}};
            e.game_frames = 4570459;
            e.game_rms = 10220.82;
            t.push_back(std::move(e));
        }
        { // VC3 1-11 Seesawing
            Entry e{"0323401760", "edc8b25618ccf1d0", "1-11", "Seesawing", "Valkyria Chronicles 3 Original Soundtrack", 175.1, 20918, Method::Exact, 21054, {}};
            e.game_frames = 3210058;
            e.game_rms = 10299.31;
            t.push_back(std::move(e));
        }
        return t;
    }();
    return table;
}

bool soundtrack_music(const std::filesystem::path &, std::vector<portablekit::audio::soundtrack::GameTrack> &,
                      std::string &) {
    return true;
}

} // namespace vc3
