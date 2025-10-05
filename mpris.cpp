#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <playerctl/playerctl.h>
#include <glib.h>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <string>
#include <vector>

template<typename T, typename... ArgsT>
inline T handle_gfunc (T(*func)(ArgsT..., GError**), ArgsT... args) {
    GError* err = nullptr;
    T result = func(std::forward<ArgsT>(args)..., &err);
    if (err != nullptr) {
        throw std::runtime_error{err->message};
    }
    return result;
}

template <typename T>
inline T g_object_get (auto *o, const char* k) {
    T value;
    g_object_get(G_OBJECT(o), k, &value, NULL);
    return value;
}
static std::string metadata_get_track_id(GVariant *metadata) {
    GVariant *track_id_variant = g_variant_lookup_value(metadata, "mpris:trackid", G_VARIANT_TYPE_OBJECT_PATH);
    if (track_id_variant == nullptr) {
        g_debug("mpris:trackid is a string, not a D-Bus object reference");
        track_id_variant = g_variant_lookup_value(metadata, "mpris:trackid", G_VARIANT_TYPE_STRING);
    }

    if (track_id_variant != nullptr) {
        const gchar *track_id = g_variant_get_string(track_id_variant, NULL);
        g_variant_unref(track_id_variant);
        return {track_id};
    }

    return {};
}

static u_int64_t metadata_get_u64_value (GVariant *metadata, const char* key) {
    GVariant *variant = g_variant_lookup_value(metadata, key, G_VARIANT_TYPE_UINT64);
    if (variant == nullptr) return {};
    return g_variant_get_uint64(variant);
}


static std::string metadata_get_str_value (GVariant *metadata, const char* key) {
    GVariant *variant = g_variant_lookup_value(metadata, key, G_VARIANT_TYPE_STRING);
    if (variant == nullptr) return {};
    const gchar *str = g_variant_get_string(variant, NULL);
    g_variant_unref(variant);
    return {str}; 
}

static std::string metadata_get_str_array_value (GVariant *metadata, const char* key) {
    GVariant *variant = g_variant_lookup_value(metadata, key, G_VARIANT_TYPE_STRING_ARRAY);
    std::string result;
    gsize prop_count = 0;
    const gchar **prop_strv = g_variant_get_strv(variant, &prop_count);

    for (gsize i = 0; i < prop_count; i++) {
        result += prop_strv[i];
        if (i != prop_count - 1) {
            result += ", ";
        }
    }

    g_free(prop_strv);
    return result;
}



struct MetadataChanges {
    bool length = false;
    bool trackid = false;
    bool title = false;
    bool album = false;
    bool artist = false;
    bool artUrl = false;
    bool url = false;

    constexpr bool none () const {
        return !(
           length
        || trackid 
        || title
        || artist
        || artUrl
        || url
        );
    }
};

struct Metadata {
    uint64_t length = 0;
    std::string trackid;
    std::string title;
    std::string album;
    std::string artist;
    std::string art_url;
    std::string url;

    constexpr MetadataChanges chnages (const Metadata& other) const {
        return {
            other.length != length,
            other.trackid != trackid,
            other.title != title,
            other.album != album,
            other.artist != artist,
            other.art_url != art_url,
            other.url != url
        };
    }
};

static Metadata parse_metadata (GVariant* variant) {
    return {
        metadata_get_u64_value(variant, "mpris:length"),
        metadata_get_track_id(variant),
        metadata_get_str_value(variant, "xesam:title"),
        metadata_get_str_value(variant, "xesam:album"),
        metadata_get_str_array_value(variant, "xesam:artist"),
        metadata_get_str_value(variant, "mpris:artUrl"),
        metadata_get_str_value(variant, "xesam:url")
    };
}

static void print_metadata (Metadata m) {
    std::cout << "\n"
    << "Track ID: " << m.trackid << "\n"
    << "Length: " << m.length << "\n"
    << "Title: " << m.title << "\n"
    << "Album: " << m.album << "\n"
    << "Artist: " << m.artist << "\n"
    << "Art Url: " << m.art_url << "\n"
    << "Url: " << m.url << "\n";
}


static void display_print (auto v) {
    std::cout << "{\"text\":\"" << v << "\"}\n";
    std::cout.flush();
}

struct _PlayerctlPlayerPrivate {
    void *proxy; // OrgMprisMediaPlayer2Player*
    gchar *player_name;
    gchar *instance;
    gchar *bus_name;
    PlayerctlSource source;
    GError *init_error;
    gboolean initted;
    PlayerctlPlaybackStatus cached_status;
    gint64 cached_position;
    gchar *cached_track_id;
    struct timespec cached_position_monotonic;
};

struct PlayerUID {
    constexpr PlayerUID () : source(PlayerctlSource::PLAYERCTL_SOURCE_NONE)  {}

    constexpr PlayerUID (PlayerctlPlayerName* name)
    :
    name(name->name),
    source(name->source)
    {}

    constexpr PlayerUID (std::string&& name, PlayerctlSource source)
    :
    name(std::move(name)),
    source(source)
    {}

    constexpr PlayerUID (const PlayerUID&)= default;
    constexpr PlayerUID (PlayerUID&&)= default;

    PlayerUID& operator = (const PlayerUID&) = default;
    PlayerUID& operator = (PlayerUID&&)= default;

    std::string name;
    PlayerctlSource source;

    constexpr bool operator == (const PlayerUID& other) const {
        return source == other.source && name == other.name;
    }

    struct hash {
        size_t operator () (const PlayerUID& v) const {
            return 
            std::hash<std::string>{}(v.name)
            | (static_cast<size_t>(v.source) << (std::numeric_limits<size_t>::digits - 4));
        }
    };
};

template <typename T>
struct GObjectWrapper {
    constexpr GObjectWrapper(T* object) : object(object) {};

    constexpr GObjectWrapper(const GObjectWrapper& other)
    : object(other.object)
    {
        g_object_ref(object);
    };

    constexpr GObjectWrapper(GObjectWrapper&& other)
    : object(other.object)
    {
        other.object = nullptr;
    };

    constexpr GObjectWrapper& operator = (const GObjectWrapper& other) {
        object = other.object;
        g_object_ref(object);
        return *this;
    };

    constexpr GObjectWrapper& operator = (GObjectWrapper&& other) {
        object = other.object;
        other.object = nullptr;
        return *this;
    };

    constexpr ~GObjectWrapper () {
        if (object != nullptr) {
            g_object_unref(object);
            object = nullptr;
        }
    }

    T* object;
};

struct UniqueOnly {
    constexpr UniqueOnly () = default;
    UniqueOnly(UniqueOnly&& other) = delete;
    UniqueOnly(const UniqueOnly& other) = delete;

    UniqueOnly& operator = (UniqueOnly&& other) = delete;
    UniqueOnly& operator = (const UniqueOnly& other) = delete;
};

#define simple_player_prop_handler(TYPE, NAME)                              \
G_CALLBACK(+[](PlayerctlPlayerManager *manager, TYPE value, Player* self) { \
    self->state.NAME = value;                                               \
    self->on_##NAME(value);                                                 \
})

struct PlayerManager;




struct Player : GObjectWrapper<PlayerctlPlayer>, UniqueOnly {

    struct State {
        Metadata metadata;
        PlayerctlLoopStatus loop_status;
        PlayerctlPlaybackStatus playback_status;
        double volume;
        uint64_t seeked_to;
        bool shuffle;
        struct Handler {
            virtual void on_state(const Player&) {};
            virtual void on_select(const Player&) {};
        };
    };

    PlayerUID uid;
    State state;
    State::Handler* state_handler;
    bool is_selected = false;

    constexpr Player (PlayerctlPlayerName* name, PlayerUID&& uid, State::Handler* state_handler)
    :
    Player{
        handle_gfunc<PlayerctlPlayer*>(playerctl_player_new_from_name, name),
        name,
        std::move(uid),
        state_handler
    }
    {}

private:
    static State create_state (PlayerctlPlayer* player) {
        return {
            parse_metadata(g_object_get<GVariant*>(player, "metadata")),
            g_object_get<PlayerctlLoopStatus>(player, "loop-status"),
            g_object_get<PlayerctlPlaybackStatus>(player, "playback-status"),
            g_object_get<gdouble>(player, "volume"),
            0,
            static_cast<bool>(g_object_get<gdouble>(player, "shuffle"))
        };
    }

    constexpr Player(PlayerctlPlayer* player, PlayerctlPlayerName* name, PlayerUID&& uid, State::Handler* state_handler)
    :
    uid(std::move(uid)),
    state(create_state(player)),
    state_handler(state_handler),
    GObjectWrapper{player}
    {
        g_signal_connect(
            G_OBJECT(object),
            "metadata",
            G_CALLBACK(+[](PlayerctlPlayerManager *manager, GVariant* variant, Player* self) {
                self->state.metadata = parse_metadata(variant);
                // print_metadata(self->state.metadata);
            }),
            this
        );
        g_signal_connect(
            object,
            "playback-status",
            simple_player_prop_handler(PlayerctlPlaybackStatus, playback_status),
            this
        );
        g_signal_connect(
            object,
            "loop-status",
            simple_player_prop_handler(PlayerctlLoopStatus, loop_status),
            this
        );
        g_signal_connect(
            object,
            "volume",
            simple_player_prop_handler(gdouble, volume),
            this
        );
        g_signal_connect(
            object,
            "shuffle",
            simple_player_prop_handler(bool, shuffle),
            this
        );

        g_signal_connect(object->priv->proxy,
            "g-properties-changed",
            G_CALLBACK(+[](
                void* proxy,
                GVariant* changed_properties,
                char** invalidated_properties,
                Player* self
            ) {
                self->state_handler->on_state(*self);
            }),
            this
        );
        g_signal_connect(object->priv->proxy,
            "g-signal::Seeked",
            G_CALLBACK(+[](
                void* proxy,
                gchar* sender_name,
                gchar* signal_name,
                GVariant* parameters,
                Player* self
            ) {
                // std::cout << "Signal: " << signal_name << "\n";
                if (parameters == nullptr) {
                    // std::cout << "Invalid seeked parameters";
                    return;
                };
                GVariant *child = g_variant_get_child_value(parameters, 0);
                if (child == nullptr) {
                    // std::cout << "Invalid seeked parameters";
                    return;
                };
                gint64 value = g_variant_get_int64(child);
                // std::cout << "Seeked to: " << value << "\n";
            }),
            this
        );
    }
public:
    void on_playback_status (PlayerctlPlaybackStatus status) {}
    void on_loop_status (PlayerctlLoopStatus status) {}
    void on_volume (double volume) {}
    void on_shuffle (bool shuffle) {}

    constexpr void select () {
        is_selected = true;
        state_handler->on_select(*this);
    }

    constexpr bool empty () const { return object == nullptr; }
};

struct ManagedPlayerHandler : Player::State::Handler {
    virtual void on_empty () = 0;
};

struct PlayerManager : GObjectWrapper<PlayerctlPlayerManager>, UniqueOnly {
    constexpr PlayerManager(ManagedPlayerHandler* handler)
    :
    GObjectWrapper{handle_gfunc(playerctl_player_manager_new)},
    handler(handler)
    {
        if (!object) {
            throw std::runtime_error{"[playerctl_player_manager_new] returned nullptr"};
        }

        GList *player_names = nullptr;
        g_object_get(object, "player-names", &player_names, NULL);
        for (GList* l = player_names; l != nullptr; l = l->next) {
            PlayerctlPlayerName *name = static_cast<PlayerctlPlayerName*>(l->data);
            assert(name != nullptr);
            // std::cout << "Found player: " << name->instance << "\n";

            add_player_by_name(name);
        }
        
        g_signal_connect(
            PLAYERCTL_PLAYER_MANAGER(object),
            "name-appeared",
            G_CALLBACK(+[](PlayerctlPlayerManager *manager, PlayerctlPlayerName *name, PlayerManager* self) {
                self->on_name_appeared(manager, name);
            }),
            this
        );

         g_signal_connect(
            PLAYERCTL_PLAYER_MANAGER(object),
            "name-vanished",
            G_CALLBACK(+[](PlayerctlPlayerManager *manager, PlayerctlPlayerName *name, PlayerManager* self) {
                self->on_name_vanished(manager, name);
            }),
            this
        );

        g_signal_connect(
            PLAYERCTL_PLAYER_MANAGER(object),
            "player-appeared",
            G_CALLBACK(on_player_appeared),
            this
        );

        g_signal_connect(
            PLAYERCTL_PLAYER_MANAGER(object),
            "player-vanished",
            G_CALLBACK(on_player_vanished),
            this
        );

        // std::cout << "[PlayerManager] initialized\n";
    }

    // Get current player list
    const Player* selected_player () const {
        if (selected_idx == NON_IDX) {
            return nullptr;
        } else {
            return managed_players[selected_idx];
        }
    }

    std::vector<Player*> managed_players; // Add player source into hash to ensure no overlaps
    static size_t constexpr NON_IDX = -1;
    size_t selected_idx = NON_IDX;
    ManagedPlayerHandler* handler = nullptr;

    // void on_state (const Player& player) override {}
    // void on_select (const Player& player) override {}

    void add_player_by_name (PlayerctlPlayerName *name) {
        PlayerUID player_uid {name->instance, name->source};
        bool exists = std::ranges::any_of(
            managed_players,
            [&player_uid](const Player* p) { return p->uid == player_uid; }
        );
        if (exists) {
            display_print("Should not exist!");
            return;
        }
        auto player = new Player{name, std::move(player_uid), handler};
        managed_players.push_back(player);
        // playerctl_player_manager_manage_player(manager, player);      

        if (selected_idx == NON_IDX) {
            player->select();
            selected_idx = managed_players.size() - 1;
            // std::cout << "selected: " << managed_players[selected_idx].object->priv->instance << "\n";
        }
    }

    void on_name_appeared (PlayerctlPlayerManager *manager, PlayerctlPlayerName *name) {
        // std::cout << "Name appeared: " << name->instance << "\n";
        add_player_by_name(name);
    }

    void on_name_vanished (PlayerctlPlayerManager *manager, PlayerctlPlayerName *name) {
        // std::cout << "Name vanished: " << name->instance << "\n";
        PlayerUID player_uid {name->instance, name->source};
        auto entry = std::ranges::find_if(managed_players, [&player_uid](const Player* p) {
            return p->uid == player_uid;
        });
        if (entry == managed_players.end()) {
            display_print("Should exist!");
            return;
        }
        bool was_selected = entry[0]->is_selected;
        delete *entry;
        managed_players.erase(entry);
        if (was_selected) {
            if (managed_players.empty()) {
                selected_idx = static_cast<size_t>(-1);
                handler->on_empty();
                return;
            }
            if (selected_idx >= managed_players.size()) {
                selected_idx = 0;
            }
        }
        managed_players[selected_idx]->select();
    }

    static void on_player_appeared (PlayerctlPlayerManager *manager, PlayerctlPlayer *player, PlayerManager* self) {
        // std::cout << "Player appeared: " << player->priv->instance << "\n";
    }

    static void on_player_vanished (PlayerctlPlayerManager *manager, PlayerctlPlayer *player, PlayerManager* self) {
        // std::cout << "Player vanished: " << player->priv->instance << "\n";
    }
};

static GMainLoop* main_loop = nullptr;

static bool handled_exit = false;
void exit_handler () {
    if (handled_exit) return;
    display_print("Exiting cleanly...");
    if (main_loop != nullptr) {
        g_main_loop_quit(main_loop);
    }
    handled_exit = true;
}

class EscapedString {
public:
    explicit EscapedString(std::string_view str)
        : data(std::move(str)) {}

    const std::string_view& get() const { return data; }

private:
    std::string_view data;

    friend std::ostream& operator<<(std::ostream& os, const EscapedString& es) {
        const std::string_view& s = es.data;
        std::size_t last = 0;

        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            std::string escape;
            switch (c) {
                case '&':  escape = "&amp;";  break;
                case '\"': escape = "&quot;"; break;
                case '\'': escape = "&apos;"; break;
                case '<':  escape = "&lt;";   break;
                case '>':  escape = "&gt;";   break;
                case '\n': escape = "\\n";    break;
                case '\t': escape = "\\t";    break;
                case '\r': escape = "\\r";    break;
            }

            if (!escape.empty()) {
                // dump everything since last safe char
                if (i > last) {
                    os.write(&s[last], i - last);
                }
                os << escape;
                last = i + 1; // move past escaped char
            }
        }

        // flush any remaining chunk
        if (last < s.size()) {
            os.write(&s[last], s.size() - last);
        }

        return os;
    }
};

std::string encode(std::string_view str) {
    std::string buffer;
    buffer.reserve(str.size());
    for(auto c : str) {
        switch(c) {
            case '&':  buffer.append("&amp;");       break;
            case '\"': buffer.append("&quot;");      break;
            case '\'': buffer.append("&apos;");      break;
            case '<':  buffer.append("&lt;");        break;
            case '>':  buffer.append("&gt;");        break;
            case '\n': buffer.append("\\n");         break;
            case '\t': buffer.append("\\t");         break;
            case '\r': buffer.append("\\r");         break;
            default:   buffer.append(1, c);          break;
        }
    }
    return std::move(buffer);
}


namespace fs = std::filesystem;

static const fs::path cache_path = fs::path{std::getenv("HOME")}/".cache/mpris-cover.png";

struct OutputGenerator : ManagedPlayerHandler {
    constexpr OutputGenerator() : manager(this) {}

    std::string to_display;
    size_t to_display_utf8_len = 0;
    bool needs_scrolling = false;
    size_t display_offset = 0;

    bool is_playing = false;

    struct LastSource {
        std::string title;
        std::string artist;
        std::string art_url;
    };
    LastSource last_src;
    
    PlayerManager manager;

    static constexpr size_t max_width = 50;

    void on_select (const Player& player) override {
        // display_print("Select recieved");
        on_update_seleceted(player);
    }

    void on_empty () override {
        // display_print("Empty recieved");
        display_print("");
        needs_scrolling = false;
    }

    void on_state (const Player& player) override {
        if (!player.is_selected) return;
        // display_print("State recieved");
        on_update_seleceted(player);
    }

    void on_update_seleceted (const Player& player) {
        auto& state = player.state;
        auto& title = state.metadata.title;
        auto& artist = state.metadata.artist;
        auto& art_url = state.metadata.art_url;
        bool new_is_playing = state.playback_status == PLAYERCTL_PLAYBACK_STATUS_PLAYING;
        if (art_url != last_src.art_url) {
            // display_print("Cover art url: " + art_url);
            update_cover_art(art_url);
        }
        if (last_src.title == title && last_src.artist == artist) {
            if (is_playing != new_is_playing) {
                is_playing = new_is_playing;
                display();
            }
        } else {
            display_offset = 0;
            last_src.title = title;
            last_src.artist = artist;
            update_to_display(title, artist);
            is_playing = new_is_playing;
            display();
        }
    }

    void update_cover_art (const std::string& art_url) {
        last_src.art_url = art_url;
        try {
            if (fs::symlink_status(cache_path).type() != fs::file_type::not_found) {
                fs::remove_all(cache_path);
            }
            if (art_url.starts_with("file://")) {
                fs::path src_path = art_url.substr(7);
                fs::create_symlink(src_path, cache_path);
            } else {
                // fs::create_symlink("/dev/null", cache_path);
            }

            // Trigger Waybar signal for module refresh (signal 5)
            int ret = std::system("pkill -RTMIN+5 waybar");
            if (ret != 0) {
                std::cerr << "Failed to send Waybar signal\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Error updating cover art: " << e.what() << "\n";
        }
    }

    void update_to_display (const std::string& title, const std::string& artist) {
        bool has_seperator = !title.empty() && !artist.empty();
        to_display_utf8_len = 
        g_utf8_strlen(title.c_str(), -1) + (has_seperator ? 3 : 0) + g_utf8_strlen(artist.c_str(), -1);
        if (to_display_utf8_len <= max_width) {
            needs_scrolling = false;
            to_display = 
            encode(title)
            + std::string{has_seperator ? " ~ " : ""}
            + encode(artist);
        } else {
            needs_scrolling = true;
            to_display = 
            title
            + std::string{has_seperator ? " ~ " : ""}
            + artist;
        }
    }

    static constexpr std::string get_state_icons (PlayerctlPlaybackStatus status) {
        switch (status) {
            case PLAYERCTL_PLAYBACK_STATUS_PLAYING: return "\uf01d";
            case PLAYERCTL_PLAYBACK_STATUS_PAUSED: return "\uf28c";
            case PLAYERCTL_PLAYBACK_STATUS_STOPPED: return "\uf28e";
            default: return "<invalid playback status>";
        }
    }

    static std::string_view utf8_substr(const std::string& str, glong start, glong length) {
        const char* begin = g_utf8_offset_to_pointer(str.c_str(), start);
        const char* end   = g_utf8_offset_to_pointer(begin, length);
        return {begin, end};
    }


    void display () {
        std::cout << "{\"text\":\"" + std::string{!is_playing ? "<i>" : ""};

        if (!needs_scrolling) {
            std::cout << to_display;
        } else {
            constexpr char seperator[] = " ~ ";
            if (display_offset < to_display_utf8_len) {
                size_t remaining = to_display_utf8_len - display_offset;
                if (remaining >= max_width) {
                    std::cout << EscapedString(utf8_substr(to_display, display_offset, max_width));
                } else {
                    std::cout << EscapedString(utf8_substr(to_display, display_offset, remaining));
                    size_t left_over = max_width - remaining;
                    if (left_over <= 3) {
                        std::cout << EscapedString({seperator, left_over});
                    } else {
                        const char* start = to_display.c_str();
                        const char* end = g_utf8_offset_to_pointer(start, left_over - 3);
                        std::cout << " ~ " << EscapedString({start, end});
                    }
                }
            } else {
                auto sperator_offset = display_offset - to_display_utf8_len;
                if (sperator_offset > 2) {
                    display_offset = 0;
                    const char* end = g_utf8_offset_to_pointer(to_display.c_str(), max_width);
                    std::cout << EscapedString({to_display.c_str(), end});
                } else {
                    size_t seperator_size = 3 - sperator_offset;
                    const char* end = g_utf8_offset_to_pointer(to_display.c_str(), max_width - seperator_size);
                    std::cout << std::string_view{seperator + sperator_offset, seperator_size} << EscapedString({to_display.c_str(), end});
                }
            }
        }

        std::cout << std::string{!is_playing ? "</i>" : ""} + "\"}\n";
        std::cout.flush();
    }

    void scoll () {
        if (!needs_scrolling) return;
        display_offset++;
        display();
    }

    static gboolean sroll_callback(gpointer self) {
        static_cast<OutputGenerator*>(self)->scoll();
        return true;
    }
};

int main (int argc, char** argv) {
    // display_print("Listening for players...");
    std::atexit(exit_handler);
    std::signal(SIGINT , [](int) { exit_handler(); });
    std::signal(SIGABRT, [](int) { exit_handler(); });
    std::signal(SIGTERM, [](int) { exit_handler(); });
    OutputGenerator output_generator;

    
    g_timeout_add(100, OutputGenerator::sroll_callback, &output_generator);

    main_loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(main_loop);

    g_main_loop_unref(main_loop);
    return 0;
}
