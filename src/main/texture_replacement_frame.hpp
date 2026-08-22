#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace texture_replacement_frame
{
    struct DrawCommand
    {
        std::string path;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        uint32_t base_texture_width = 0;
        uint32_t base_texture_height = 0;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
        bool repeat = false;
        bool flip_x = false;
        bool flip_y = false;

        // Full-frame tile/road commands use exact indexed-pixel ownership.
        // Sprite commands instead identify their final pixels by System-16
        // palette base (plus optional shadow-half ownership).
        bool sprite_palette_ownership = false;
        uint16_t sprite_palette_base = 0;
        bool sprite_shadow = false;

        // These arrays are command-local: width * height entries. Full-screen
        // layer commands simply have x=y=0 and dimensions equal to the frame.
        std::vector<uint16_t> expected_pixels;
        std::vector<uint16_t> restore_pixels;
        std::vector<uint8_t> visibility_mask;
    };

    struct Frame
    {
        int logical_width = 0;
        int logical_height = 0;
        std::vector<DrawCommand> commands;
    };

    // Check each generated replacement filename only once per run. Restarting
    // CannonBall is enough after adding/removing a file while developing.
    inline std::unordered_map<std::string, bool> availability_cache;

    inline bool replacement_exists(const std::string& path)
    {
        const auto found = availability_cache.find(path);
        if (found != availability_cache.end())
            return found->second;

        std::error_code ec;
        const bool exists = std::filesystem::is_regular_file(path, ec);
        availability_cache.emplace(path, exists && !ec);
        return exists && !ec;
    }

    // CannonBall-SE prepares/presents from two alternating buffers. Keep the
    // replacement command stream aligned with those same slots.
    inline std::mutex queue_mutex;
    inline std::deque<Frame> prepared[2];
    inline bool present_seen[2] = { false, false };
    inline int present_slot = 0;

    inline void publish_prepared(int slot, Frame frame)
    {
        if (slot < 0 || slot > 1)
            return;

        std::lock_guard<std::mutex> lock(queue_mutex);
        prepared[slot].push_back(std::move(frame));
        while (prepared[slot].size() > 4)
            prepared[slot].pop_front();
    }

    inline Frame consume_for_present()
    {
        std::lock_guard<std::mutex> lock(queue_mutex);

        const int slot = present_slot;
        present_slot ^= 1;

        if (!present_seen[slot])
        {
            present_seen[slot] = true;
            return {};
        }

        if (prepared[slot].empty())
            return {};

        Frame frame = std::move(prepared[slot].front());
        prepared[slot].pop_front();
        return frame;
    }

    inline void reset()
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        prepared[0].clear();
        prepared[1].clear();
        present_seen[0] = false;
        present_seen[1] = false;
        present_slot = 0;
        availability_cache.clear();
    }
}
