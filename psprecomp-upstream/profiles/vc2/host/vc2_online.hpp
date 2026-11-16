#pragma once

// Online play for VC2's co-op and Versus modes (online/online_session.hpp),
// set from the launcher's Network page (vc2.online, vc2.online_key,
// vc2.online_my_code, vc2.online_friend1..3; vc2_settings.hpp).
//
// Friend codes: each player makes one in the launcher (vc2-online-code.exe:
// their public key and the addresses their PC can be reached at) and pastes
// up to three friends'. Two players connect only when each has the other's
// code; their session's secret comes from one's key and the other's public key.
//
// One PC runs the session: the one whose code sorts first, which every player
// works out alike. It starts the built-in ad hoc server on 127.0.0.1 only and
// the online session on UDP 27320 (vc2.online_port), maps the port on the
// router (UPnP, else NAT-PMP) and keeps sending PUNCHes to the friends who are
// not yet connected (their codes say where), so both routers open. The others
// connect to it, from the same port, and their games' ad hoc clients are
// pointed at the session's local end.
//
// Nothing is drawn over the game: the window's title says who is connected,
// and when this PC's code is out of date (the public address changed).
//
// In-game, the multiplayer menus work as on PSPs among friends: anyone makes
// the room, the others find it in their list.
namespace portablekit {
class HleRegistrar;
}

namespace portablekit::vc2 {

// From the profile's install, once.
void install_online();

// True when online play is on (host or join): the game must then not host a
// session of its own when it goes on line (vc2_profile.cpp).
[[nodiscard]] bool online_active();

// When the game goes on line (sceNetAdhocctlInit): the host's built-in server,
// on 127.0.0.1 only, or the joiner's game pointed at the session's local end.
// False when online play is off (the game's usual local session then).
bool online_prepare_adhoc();

} // namespace portablekit::vc2
