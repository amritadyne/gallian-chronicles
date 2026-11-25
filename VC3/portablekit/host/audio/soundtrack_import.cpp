#include "audio/soundtrack_import.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace portablekit::audio::soundtrack {

namespace {

using Complex = std::complex<double>;

// Offsets this close to the table's are the reference rip itself (rounding apart).
constexpr std::int64_t kSnap = 2;

void fft(std::vector<Complex> &a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2.0 * 3.14159265358979323846 / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const Complex step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            Complex w(1.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const Complex u = a[i + k];
                const Complex v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
    if (inverse)
        for (Complex &x : a) x /= static_cast<double>(n);
}

// Mono mix of frames [from, from + count), zero outside the track, averaged
// over `decimate` frames.
std::vector<double> mono(const Stereo &x, std::int64_t from, std::int64_t count, int decimate) {
    const std::int64_t out_count = count / decimate;
    std::vector<double> out(static_cast<std::size_t>(std::max<std::int64_t>(out_count, 0)), 0.0);
    const auto frames = static_cast<std::int64_t>(x.frames());
    for (std::int64_t i = 0; i < out_count; ++i) {
        double sum = 0.0;
        for (int k = 0; k < decimate; ++k) {
            const std::int64_t f = from + i * decimate + k;
            if (f >= 0 && f < frames) sum += 0.5 * (static_cast<double>(x.left(f)) + x.right(f));
        }
        out[static_cast<std::size_t>(i)] = sum / decimate;
    }
    return out;
}

double rms(const Stereo &x, std::int64_t from, std::int64_t to) {
    from = std::max<std::int64_t>(from, 0);
    to = std::min<std::int64_t>(to, static_cast<std::int64_t>(x.frames()));
    if (to <= from) return 0.0;
    double sum = 0.0;
    for (std::int64_t i = 2 * from; i < 2 * to; ++i) sum += static_cast<double>(x.samples[i]) * x.samples[i];
    return std::sqrt(sum / static_cast<double>(2 * (to - from)));
}

double peak(const Stereo &x) {
    double p = 0.0;
    for (float v : x.samples) p = std::max(p, static_cast<double>(std::fabs(v)));
    return p;
}

std::int64_t median(std::vector<std::int64_t> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[(v.size() - 1) / 2] + (v.size() % 2 == 0 ? (v[v.size() / 2] - v[(v.size() - 1) / 2]) / 2 : 0);
}

// Points spread evenly from 3 s to 4 s before the end (or across a short span).
std::vector<std::int64_t> points(std::int64_t begin, std::int64_t end, int count, std::int64_t window) {
    std::int64_t first = begin + 3 * kRate;
    std::int64_t last = end - 4 * kRate;
    if (last <= first) {
        first = begin;
        last = std::max(begin, end - window);
    }
    std::vector<std::int64_t> out;
    for (int i = 0; i < count; ++i)
        out.push_back(count == 1 ? first
                                 : first + static_cast<std::int64_t>(std::llround(
                                               static_cast<double>(last - first) * i / (count - 1))));
    return out;
}

// album[p + lag] for p in [0, frames), zero where the album has nothing.
Stereo placed(const Stereo &album, std::int64_t lag, std::size_t frames) {
    Stereo out;
    out.samples.assign(frames * 2, 0.0f);
    const auto have = static_cast<std::int64_t>(album.frames());
    for (std::int64_t p = 0; p < static_cast<std::int64_t>(frames); ++p) {
        const std::int64_t a = p + lag;
        if (a < 0 || a >= have) continue;
        out.samples[2 * p] = album.samples[2 * a];
        out.samples[2 * p + 1] = album.samples[2 * a + 1];
    }
    return out;
}

// The coarse offset: one window, decimated, wide search; then `count` points at
// full rate around it. Returns the fine lags and scores.
struct Fine {
    std::vector<std::int64_t> lags;
    std::vector<double> scores;
};

Fine fine_lags(const Stereo &game, const Stereo &album, std::int64_t begin, std::int64_t end, std::int64_t lag_guess,
               int count, std::int64_t fine_search) {
    const std::int64_t span = end - begin;
    const std::int64_t coarse_window = std::min<std::int64_t>(6 * kRate, span);
    const std::int64_t coarse_at = begin + std::max<std::int64_t>(0, std::min<std::int64_t>(3 * kRate, span - coarse_window));
    const LagFound coarse = find_lag(game, album, coarse_at, coarse_window, lag_guess, 2 * kRate, 4);
    Fine fine;
    const std::int64_t window = std::min<std::int64_t>(2 * kRate, span);
    for (std::int64_t at : points(begin, end, count, window)) {
        const LagFound f = find_lag(game, album, at, window, coarse.lag, fine_search, 1);
        if (f.score <= -1.0) continue;
        fine.lags.push_back(f.lag);
        fine.scores.push_back(f.score);
    }
    return fine;
}

void add_section(Stereo &out, const Stereo &piece_source, std::int64_t source_lag, std::int64_t from, std::int64_t to,
                 std::int64_t fade, bool fade_in, bool fade_out, bool equal_power, double level) {
    const auto frames = static_cast<std::int64_t>(out.frames());
    const std::int64_t s0 = std::max<std::int64_t>(fade_in ? from - fade : from, 0);
    const std::int64_t s1 = std::min<std::int64_t>(fade_out ? to + fade : to, frames);
    const std::int64_t n = s1 - s0;
    const auto have = static_cast<std::int64_t>(piece_source.frames());
    for (std::int64_t i = 0; i < n; ++i) {
        double w = 1.0;
        if (fade_in && i < 2 * fade) {
            const double t = static_cast<double>(i) / static_cast<double>(2 * fade - 1);
            w = equal_power ? std::pow(std::sin(t * 3.14159265358979323846 / 2), 2) : t;
        }
        if (fade_out && i >= n - 2 * fade) {
            const double t = static_cast<double>(i - (n - 2 * fade)) / static_cast<double>(2 * fade - 1);
            w *= equal_power ? std::pow(std::cos(t * 3.14159265358979323846 / 2), 2) : 1.0 - t;
        }
        const std::int64_t p = s0 + i;
        const std::int64_t a = p + source_lag;
        if (a < 0 || a >= have) continue;
        out.samples[2 * p] += static_cast<float>(piece_source.samples[2 * a] * w * level);
        out.samples[2 * p + 1] += static_cast<float>(piece_source.samples[2 * a + 1] * w * level);
    }
}

std::vector<std::int16_t> to_pcm16(const Stereo &x, double gain) {
    std::vector<std::int16_t> out(x.samples.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::int16_t>(std::clamp(std::nearbyint(x.samples[i] * gain), -32768.0, 32767.0));
    return out;
}

// Gain that matches `x` to the game's level, at most 0.97 of full scale at its peak.
double level_gain(const Stereo &game, const Stereo &x) {
    const double want = rms(game, 0, static_cast<std::int64_t>(game.frames()));
    const double have = rms(x, 0, static_cast<std::int64_t>(x.frames()));
    const double cap = 0.97 * 32767.0 / std::max(peak(x), 1.0);
    return std::min(want / std::max(have, 1e-9), cap);
}

Result one_offset(const Entry &entry, const Stereo &game, const Stereo &album) {
    Result result;
    const auto n = static_cast<std::int64_t>(game.frames());
    const bool exact = entry.method == Method::Exact;
    const Fine fine = fine_lags(game, album, 0, n, entry.lag, exact ? 5 : 9, exact ? 256 : kRate / 2);
    if (fine.lags.size() < 3) {
        result.reason = "too short to compare";
        return result;
    }
    const std::int64_t measured = median(fine.lags);
    // Within a couple of samples of the table: the same rip, rounding apart.
    const std::int64_t mid = std::llabs(measured - entry.lag) <= kSnap ? entry.lag : measured;
    if (exact) {
        int agree = 0;
        double weakest = 1.0;
        for (std::size_t i = 0; i < fine.lags.size(); ++i)
            if (std::llabs(fine.lags[i] - measured) <= 4) {
                ++agree;
                weakest = std::min(weakest, fine.scores[i]);
            }
        if (agree < 3 || weakest < 0.45) {
            result.reason = "the recording does not line up with the game's";
            return result;
        }
        result.score = weakest;
    } else {
        std::vector<std::int64_t> spread = fine.lags;
        std::sort(spread.begin(), spread.end());
        const std::int64_t iqr = spread[spread.size() * 3 / 4] - spread[spread.size() / 4];
        const double best = *std::max_element(fine.scores.begin(), fine.scores.end());
        if (iqr > kRate / 50 || best < 0.2) {
            result.reason = "the recording does not keep the game's timing";
            return result;
        }
        result.score = best;
    }
    result.lag = mid;
    result.rip_offset = mid - entry.lag;
    const Stereo out = placed(album, mid, game.frames());
    const double gain = level_gain(game, out);
    result.gain_db = 20.0 * std::log10(std::max(gain, 1e-9));
    result.pcm = to_pcm16(out, gain);
    result.ok = true;
    return result;
}

Result from_recipe(const Entry &entry, const Stereo &game, const Stereo &album, const Stretch &stretch) {
    Result result;
    const Recipe &recipe = entry.recipe;
    const auto n = static_cast<std::int64_t>(game.frames());
    // How this rip differs from the reference: measured on the first album section.
    const auto first = std::find_if(recipe.sections.begin(), recipe.sections.end(),
                                    [](const Section &s) { return s.source == Section::Source::Album; });
    if (first == recipe.sections.end()) {
        result.reason = "no album section";
        return result;
    }
    std::int64_t found = first->lag;
    double weakest = 1.0;
    if (recipe.loose) {
        const std::int64_t start = onset(album);
        if (start < 0) {
            result.reason = "the album track is silent";
            return result;
        }
        found = first->lag + (start - entry.album_onset);
        const std::int64_t span = first->to - first->from;
        const std::int64_t window = std::min<std::int64_t>(6 * kRate, span);
        const std::int64_t at = first->from + std::max<std::int64_t>(0, std::min<std::int64_t>(3 * kRate, span - window));
        const LagFound check = find_lag(game, album, at, window, found, kRate / 2, 4);
        if (check.score < 0.3) {
            result.reason = "the recording does not line up with the game's";
            return result;
        }
        weakest = check.score;
    } else {
        const Fine fine = fine_lags(game, album, first->from, first->to, first->lag, 3, 256);
        if (fine.lags.size() < 2) {
            result.reason = "too short to compare";
            return result;
        }
        found = median(fine.lags);
        int agree = 0;
        for (std::size_t i = 0; i < fine.lags.size(); ++i)
            if (std::llabs(fine.lags[i] - found) <= 4) {
                ++agree;
                weakest = std::min(weakest, fine.scores[i]);
            }
        if (agree < 2 || weakest < 0.45) {
            result.reason = "the recording does not line up with the game's";
            return result;
        }
    }
    std::int64_t offset = found - first->lag;
    if (std::llabs(offset) <= kSnap) offset = 0;
    result.lag = found;
    result.rip_offset = offset;
    result.score = weakest;

    Stereo out;
    out.samples.assign(game.samples.size(), 0.0f);
    for (std::size_t i = 0; i < recipe.sections.size(); ++i) {
        const Section &s = recipe.sections[i];
        const bool fade_in = i > 0;
        const bool fade_out = i + 1 < recipe.sections.size();
        const Stereo *source = &album;
        std::int64_t lag = s.lag + offset;
        Stereo stretched;
        if (s.source == Section::Source::Game) {
            source = &game;
            lag = 0;
        } else if (s.source == Section::Source::AlbumStretched) {
            if (!stretch) {
                result.reason = "no tempo change available";
                return result;
            }
            const auto start = static_cast<std::int64_t>(std::llround(s.album_start * kRate)) + offset;
            Stereo tail;
            const std::int64_t from = std::clamp<std::int64_t>(start, 0, static_cast<std::int64_t>(album.frames()));
            tail.samples.assign(album.samples.begin() + 2 * from, album.samples.end());
            stretched = stretch(tail, s.tempo);
            if (stretched.frames() == 0) {
                result.reason = "the tempo change failed";
                return result;
            }
            // Stretched sample q plays at game position from + q - lag: the table's placement.
            lag = s.lag - s.from;
            if (s.search > 0) {
                // Or where it matches the game's own audio best (only for a clear match).
                const std::int64_t window = std::min<std::int64_t>(5 * kRate, (s.to - s.from) / 2);
                const LagFound at = find_lag(game, stretched, s.from + kRate, window, lag, s.search, 1);
                if (at.score < 0.15) {
                    result.reason = "the tempo-changed part does not line up with the game's";
                    return result;
                }
                lag = at.lag;
                result.score = std::min(result.score, at.score);
            }
            source = &stretched;
        }
        double level = 1.0;
        if (recipe.level_each_section && s.source != Section::Source::Game) {
            Stereo piece = placed(*source, lag, game.frames());
            level = rms(game, s.from, s.to) / std::max(rms(piece, s.from, s.to), 1e-9);
        }
        add_section(out, *source, lag, s.from, s.to, recipe.crossfade, fade_in, fade_out, recipe.equal_power, level);
    }
    const double gain = level_gain(game, out);
    for (float &v : out.samples) v = static_cast<float>(v * gain);
    if (recipe.join_half > 0) {
        const double game_level = rms(out, 0, n) / std::max(rms(game, 0, n), 1e-9);
        for (std::size_t i = 1; i < recipe.sections.size(); ++i) {
            const std::int64_t cut = recipe.sections[i].from;
            const std::int64_t s0 = std::max<std::int64_t>(cut - recipe.join_half, 0);
            const std::int64_t s1 = std::min<std::int64_t>(cut + recipe.join_half, n);
            const std::int64_t len = s1 - s0;
            const std::int64_t x = recipe.join_fade;
            for (std::int64_t k = 0; k < len; ++k) {
                double w = 1.0;
                if (k < x) w = static_cast<double>(k) / static_cast<double>(x - 1);
                if (k >= len - x) w = 1.0 - static_cast<double>(k - (len - x)) / static_cast<double>(x - 1);
                const std::int64_t p = s0 + k;
                for (int c = 0; c < 2; ++c)
                    out.samples[2 * p + c] = static_cast<float>(out.samples[2 * p + c] * (1.0 - w) +
                                                                game.samples[2 * p + c] * game_level * w);
            }
        }
    }
    result.gain_db = 20.0 * std::log10(std::max(gain, 1e-9));
    result.pcm = to_pcm16(out, 1.0);
    result.ok = true;
    return result;
}

} // namespace

LagFound find_lag(const Stereo &game, const Stereo &album, std::int64_t at, std::int64_t length, std::int64_t lag_guess,
                  std::int64_t search, int decimate) {
    LagFound best;
    if (length <= 0 || at < 0 || at + length > static_cast<std::int64_t>(game.frames())) return best;
    const std::vector<double> seg = mono(game, at, length, decimate);
    const std::int64_t ref_from = at + lag_guess - search;
    const std::vector<double> ref = mono(album, ref_from, length + 2 * search, decimate);
    const std::size_t m = seg.size();
    const std::size_t r = ref.size();
    if (m == 0 || r < m) return best;
    std::size_t size = 1;
    while (size < r + m) size <<= 1;
    std::vector<Complex> a(size), b(size);
    for (std::size_t i = 0; i < r; ++i) a[i] = ref[i];
    for (std::size_t i = 0; i < m; ++i) b[i] = seg[i];
    fft(a, false);
    fft(b, false);
    for (std::size_t i = 0; i < size; ++i) a[i] *= std::conj(b[i]);
    fft(a, true);
    double seg_energy = 0.0;
    for (double v : seg) seg_energy += v * v;
    std::vector<double> cumulative(r + 1, 0.0);
    for (std::size_t i = 0; i < r; ++i) cumulative[i + 1] = cumulative[i] + ref[i] * ref[i];
    double best_score = -2.0;
    std::size_t best_at = 0;
    for (std::size_t k = 0; k + m <= r; ++k) {
        const double energy = std::sqrt((cumulative[k + m] - cumulative[k]) * seg_energy) + 1e-9;
        const double score = a[k].real() / energy;
        if (score > best_score) {
            best_score = score;
            best_at = k;
        }
    }
    best.lag = ref_from + static_cast<std::int64_t>(best_at) * decimate - at;
    best.score = best_score;
    return best;
}

std::int64_t onset(const Stereo &album) {
    for (std::size_t i = 0; i < album.frames(); ++i)
        if (std::fabs(album.left(i)) >= kOnsetLevel || std::fabs(album.right(i)) >= kOnsetLevel)
            return static_cast<std::int64_t>(i);
    return -1;
}

double quick_score(const Entry &entry, const Stereo &game, const Stereo &album) {
    std::int64_t begin = 0;
    std::int64_t end = static_cast<std::int64_t>(game.frames());
    std::int64_t lag = entry.lag;
    if (entry.method == Method::Recipe) {
        for (const Section &s : entry.recipe.sections)
            if (s.source == Section::Source::Album) {
                begin = s.from;
                end = s.to;
                lag = s.lag;
                break;
            }
    }
    const std::int64_t span = end - begin;
    if (span < kRate) return -1.0;
    const std::int64_t window = std::min<std::int64_t>(6 * kRate, span);
    const std::int64_t at = begin + std::max<std::int64_t>(0, std::min<std::int64_t>(3 * kRate, span - window));
    return find_lag(game, album, at, window, lag, 2 * kRate, 4).score;
}

Result build_from_reference(const Entry &entry, const Stereo &album) {
    Result result;
    if (entry.game_frames <= 0 || entry.game_rms <= 0.0 || album.frames() == 0) {
        result.reason = "no audio";
        return result;
    }
    const double seconds = static_cast<double>(album.frames()) / kRate;
    if (std::fabs(seconds - entry.album_seconds) > 3.0) {
        result.reason = "the album track is not the expected length";
        return result;
    }
    const std::int64_t start = onset(album);
    if (start < 0) {
        result.reason = "the album track is silent";
        return result;
    }
    std::int64_t offset = start - entry.album_onset;
    if (std::llabs(offset) <= kSnap) offset = 0;
    if (std::llabs(offset) > 2 * kRate) {
        result.reason = "the album track starts too far from where it should";
        return result;
    }
    const auto frames = static_cast<std::size_t>(entry.game_frames);
    Stereo out;
    if (entry.method == Method::Recipe) {
        out.samples.assign(frames * 2, 0.0f);
        const auto &sections = entry.recipe.sections;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (sections[i].source != Section::Source::Album) {
                result.reason = "this track needs the game's own audio";
                return result;
            }
            add_section(out, album, sections[i].lag + offset, sections[i].from, sections[i].to, entry.recipe.crossfade,
                        i > 0, i + 1 < sections.size(), entry.recipe.equal_power, 1.0);
        }
        result.lag = sections.empty() ? 0 : sections.front().lag + offset;
    } else {
        out = placed(album, entry.lag + offset, frames);
        result.lag = entry.lag + offset;
    }
    const double have = rms(out, 0, static_cast<std::int64_t>(frames));
    const double cap = 0.97 * 32767.0 / std::max(peak(out), 1.0);
    const double gain = std::min(entry.game_rms / std::max(have, 1e-9), cap);
    result.rip_offset = offset;
    result.score = 1.0;
    result.gain_db = 20.0 * std::log10(std::max(gain, 1e-9));
    result.pcm = to_pcm16(out, gain);
    result.ok = true;
    return result;
}

Result build(const Entry &entry, const Stereo &game, const Stereo &album, const Stretch &stretch) {
    if (game.frames() == 0 || album.frames() == 0) {
        Result r;
        r.reason = "no audio";
        return r;
    }
    if (entry.method == Method::Recipe) return from_recipe(entry, game, album, stretch);
    return one_offset(entry, game, album);
}

} // namespace portablekit::audio::soundtrack
