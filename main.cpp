#include <Stuff/Util/Hacks/Coroutines.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <shared_mutex>

#include <SFML/Graphics.hpp>
#include <SFML/System/Clock.hpp>
#include <SFML/Window.hpp>
#include <boost/asio.hpp>
#include <crow.h>
#include <imgui-SFML.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <Stuff/IO/Delim.hpp>
#include <Stuff/IO/Packetman.hpp>
#include <Stuff/Maths/Maths.hpp>
#include <Uncommon/CEPackets.hpp>

using namespace std::chrono_literals;

namespace asio = boost::asio;
using asio::awaitable;
using asio::co_spawn;

struct Connection : public Stf::PacketManagerBase<512, 512> {
    Connection(asio::io_context& io_ctx)
        : m_io_ctx(io_ctx)
        , m_serial_port(m_io_ctx.get_executor()) {
        const std::string_view essentials_base{"tele_essential"};
        const std::string_view verbose_base{"tele_verbose"};

        const auto now = std::chrono::system_clock::now();
        const auto now_t = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm{};
        std::ignore = localtime_r(&now_t, &local_tm);

        const auto timestamp = fmt::format("{:02}{:02}{:02}_{:02}{:02}{:02}",
                                           (local_tm.tm_year + 1900) % 100,
                                           local_tm.tm_mon,
                                           local_tm.tm_mday,
                                           local_tm.tm_hour,
                                           local_tm.tm_min,
                                           local_tm.tm_sec);

        const auto essentials_filename = fmt::format("{}-{}.csv", essentials_base, timestamp);
        const auto verbose_filename = fmt::format("{}-{}.csv", verbose_base, timestamp);

        essentials_ofs.open(essentials_filename);
        verbose_ofs.open(verbose_filename);

        if (!essentials_ofs)
            throw std::runtime_error("failed to open essentials log for writing");

        if (!verbose_ofs)
            throw std::runtime_error("failed to open verbose log for writing");
    };

    void dump_verbose() {
        // clang-format off
        verbose_ofs
            << last_contact() << ","
            << out_last_bms_summary_packet.min_max_avg_cell_voltages[0] << ","
            << out_last_bms_summary_packet.min_max_avg_cell_voltages[1] << ","
            << out_last_bms_summary_packet.min_max_avg_cell_voltages[2] << ","
            << out_last_bms_summary_packet.cell_sum << ","
            << out_last_bms_summary_packet.spent_mah << ","
            << out_last_bms_summary_packet.spent_mwh << ","
            << out_last_bms_summary_packet.soc_percent << ","
            << out_last_bms_summary_packet.current << ","
            << out_last_engine_packet.speed << ","
            << out_last_engine_packet.rpm << ","
            << out_last_engine_packet.reported_duty << ","
            << out_last_debug_metrics_packet.performance_report[0] << ","
            << out_last_debug_metrics_packet.performance_report[1] << ","
            << out_last_debug_metrics_packet.performance_report[2] << ",";
        // clang-format on

        for (auto b: out_last_battery_array_packet.voltages) {
            verbose_ofs << b << ",";
        }

        for (auto b: out_last_bms_summary_packet.battery_temperatures) {
            verbose_ofs << b << ",";
        }

        verbose_ofs << "," << std::endl;
        verbose_ofs.flush();
    }

    void dump_essential() {
        // clang-format off
        essentials_ofs
            << out_last_essential_packet.timestamp << ","
            << out_last_essential_packet.speed << ","
            << out_last_essential_packet.max_temp << ","
            << out_last_essential_packet.cell_sum << ","
            << (1458 - out_last_essential_packet.spent_wh) << ","
            << out_last_bms_summary_packet.soc_percent << ","
            << out_last_essential_packet.spent_wh << ","
            << out_last_essential_packet.spent_ah << ",";
        // clang-format on

        essentials_ofs << "," << std::endl;

        essentials_ofs.flush();
    }

    void connect(std::string const& file) {
        m_serial_port.open(file);
        m_serial_port.set_option(boost::asio::serial_port_base::baud_rate(115200));

        begin_receive();
    }

    void disconnect() {
        m_serial_port.close();
    }

    bool is_connected() const {
        return m_serial_port.is_open();
    }

    void tx_buffer(uint8_t* b, size_t n) override {
        static std::array<uint8_t, 3> preamble{0, 64, 23};

        auto buf_ptr = std::make_unique<uint8_t[]>(n);
        std::copy(b, b + n, buf_ptr.get());

        std::unique_lock transmit_lock{transmit_mutex};
        asio::async_write(m_serial_port, asio::buffer(preamble), [&, transmit_lock = std::move(transmit_lock), buf_ptr = std::move(buf_ptr), n](boost::system::error_code const& ec, size_t) mutable {
            if (ec.failed())
                return;
            asio::async_write(m_serial_port, asio::buffer(buf_ptr.get(), n), [transmit_lock = std::move(transmit_lock), buf_ptr = std::move(buf_ptr)](boost::system::error_code const& ec, size_t) {
                //
            });
        });
    }

    void rx_packet(std::span<const uint8_t> p, Stf::PacketHeader header) override {
#define PACKET_HANDLING_CASE(type, ...)                                         \
    case type::packet_id: {                                                     \
        const auto expected_size = Stf::serialized_size_v<type>;                \
        if (header.len != expected_size) {                                      \
            ++stats.length_errors;                                              \
            break;                                                              \
        }                                                                       \
                                                                                \
        auto packet = Stf::deserialize<type>(p.begin());                        \
                                                                                \
        if constexpr (!std::is_same_v<std::nullptr_t, decltype(__VA_ARGS__)>) { \
            auto& out = __VA_ARGS__;                                            \
            std::unique_lock lock(out_packets_mutex);                           \
            out = packet;                                                       \
        }                                                                       \
        new_packet(std::move(packet), header);                                  \
                                                                                \
        break;                                                                  \
    }

        switch (header.id) {
            PACKET_HANDLING_CASE(Unc::VCSPacket, out_last_vcs_packet);
            PACKET_HANDLING_CASE(Unc::BMSSummaryPacket, out_last_bms_summary_packet);
            PACKET_HANDLING_CASE(Unc::EnginePacket, out_last_engine_packet);
            PACKET_HANDLING_CASE(Unc::LocalObservationsPacket, out_last_local_observations_packet);
            PACKET_HANDLING_CASE(Unc::BatteryArrayPacket, out_last_battery_array_packet);
            PACKET_HANDLING_CASE(Unc::DebugMetricsPacket, out_last_debug_metrics_packet);
            PACKET_HANDLING_CASE(Unc::MemoryResponsePacket8B, std::ignore);
            PACKET_HANDLING_CASE(Unc::MemoryResponsePacket16B, std::ignore);
            PACKET_HANDLING_CASE(Unc::MemoryResponsePacket32B, std::ignore);
            PACKET_HANDLING_CASE(Unc::MemoryResponsePacket64B, std::ignore);
            PACKET_HANDLING_CASE(Unc::EssentialsPacket, out_last_essential_packet);
            default:
                break;
        }

        dump_verbose();
    }

    void new_packet(Unc::VCSPacket packet, Stf::PacketHeader header) {}
    void new_packet(Unc::BMSSummaryPacket packet, Stf::PacketHeader header) {}
    void new_packet(Unc::EnginePacket packet, Stf::PacketHeader header) {}
    void new_packet(Unc::LocalObservationsPacket packet, Stf::PacketHeader header) {}
    void new_packet(Unc::BatteryArrayPacket packet, Stf::PacketHeader header) {}
    void new_packet(Unc::DebugMetricsPacket packet, Stf::PacketHeader header) {}

    void new_packet(Unc::EssentialsPacket packet, Stf::PacketHeader header) {
        dump_essential();
    }

    template<typename T>
        requires(requires {
                     { std::declval<T>().address } -> std::convertible_to<uint32_t>;
                 })
    void new_packet(T packet, Stf::PacketHeader header) {
        using U = decltype(packet.data);

        fmt::print("{:08X}:{:08X}: ", packet.address, packet.address + sizeof(U));
        for (size_t i = 0; i < sizeof(U); i++) {
            const auto shift = std::numeric_limits<U>::digits - 8;
            fmt::print("{:02X}", static_cast<uint8_t>(packet.data >> shift));
            packet.data <<= 8;
        }
        fmt::print("\n");
    }

    float last_contact() const {
        std::shared_lock out_lock(out_packets_mutex);

        std::array<float, 6> timestamps{
          out_last_vcs_packet.timestamp,
          out_last_bms_summary_packet.timestamp,
          out_last_engine_packet.timestamp,
          out_last_local_observations_packet.timestamp,
          out_last_battery_array_packet.timestamp,
          out_last_debug_metrics_packet.timestamp,
        };

        /*std::sort(timestamps.begin(), timestamps.end());
        return timestamps.back();*/
        return *std::max_element(timestamps.cbegin(), timestamps.cend());
    };

    mutable std::shared_mutex out_packets_mutex{};

    Unc::VCSPacket out_last_vcs_packet;
    Unc::BMSSummaryPacket out_last_bms_summary_packet;
    Unc::EnginePacket out_last_engine_packet;
    Unc::LocalObservationsPacket out_last_local_observations_packet;
    Unc::BatteryArrayPacket out_last_battery_array_packet;
    Unc::DebugMetricsPacket out_last_debug_metrics_packet;
    Unc::EssentialsPacket out_last_essential_packet;

private:
    asio::io_context& m_io_ctx;

    boost::asio::basic_serial_port<asio::io_context::executor_type> m_serial_port;
    mutable std::mutex transmit_mutex{};
    std::array<uint8_t, 512> receive_buffer{};

    std::ofstream essentials_ofs;
    std::ofstream verbose_ofs;

    void begin_receive() {
        m_serial_port.async_read_some(asio::buffer(receive_buffer), [this](boost::system::error_code const& ec, size_t sz) {
            receiver(ec, std::span(receive_buffer.data(), sz));
        });
    }

    void receiver(boost::system::error_code const& ec, std::span<const uint8_t> data) {
        if (ec.failed())
            return;

        //fmt::print("Received {} bytes", data.size());
        for (auto b: data)
            rx_byte(b);

        begin_receive();
    }
};

struct State {
    State(asio::io_context& io_ctx, sf::RenderWindow& window)
        : m_io_ctx(io_ctx)
        , m_connection(io_ctx)
        , m_render_timer(io_ctx, k_frame_time)
        , m_window(window) {
        m_window.setFramerateLimit(0);

        if (!ImGui::SFML::Init(m_window))
            throw std::runtime_error("could not initialize imgui");

        do_render();
    }

    ~State() noexcept {
        m_connection.disconnect();
        ImGui::SFML::Shutdown();
    }

private:
    const boost::posix_time::milliseconds k_frame_time{16};

    asio::io_context& m_io_ctx;

    Connection m_connection;

    sf::Clock m_render_clock{};
    sf::RenderWindow& m_window;
    boost::asio::deadline_timer m_render_timer;

    std::vector<std::filesystem::directory_entry> m_enumerated_devices{};

    void enumerate_devices() {
        const std::filesystem::directory_iterator dir_iter{"/sys/class/tty"};
        std::vector<std::filesystem::directory_entry> entries{begin(dir_iter), end(dir_iter)};
        std::sort(entries.begin(), entries.end(), [](auto const& lhs, auto const& rhs) -> bool {
            return std::less<std::filesystem::path>{}(lhs.path(), rhs.path());
        });
        m_enumerated_devices = entries;
    }

    struct {
        //connection controls
        std::string serial_filename{};
        int serial_selection = 0;
        std::array<uint32_t, 2> requested_range{0, 0};
    } m_inputs;

    void do_render() {
        m_render_timer.async_wait([this](boost::system::error_code const& ec) {
            render_frame(ec);
        });
    }

    void render_frame(boost::system::error_code const&) {
        std::ignore = m_window.setActive(true);
        if (!m_window.isOpen())
            return;

        const auto elapsed_time = m_render_clock.restart();

        sf::Event event;
        while (m_window.pollEvent(event)) {
            ImGui::SFML::ProcessEvent(m_window, event);
            if (event.type == sf::Event::Closed) {
                m_window.close();
            }
        }

        ImGui::SFML::Update(m_window, elapsed_time);

        ImGui::Begin("connection controls");
        {
            ImGui::InputText("Serial port", &m_inputs.serial_filename);

            std::vector<std::string> serial_devices_std{};
            serial_devices_std.reserve(m_enumerated_devices.size());
            for (auto const& entry: m_enumerated_devices)
                serial_devices_std.push_back(entry.path().filename().string());

            std::vector<const char*> serial_devices{};
            serial_devices.reserve(m_enumerated_devices.size());
            for (auto const& s: serial_devices_std)
                serial_devices.push_back(s.c_str());

            if (ImGui::ListBox("Serial devices", &m_inputs.serial_selection, serial_devices.data(), static_cast<int>(serial_devices.size()))) {
                m_inputs.serial_filename = "/dev/" + serial_devices_std.at(m_inputs.serial_selection);
            }

            bool do_connect = ImGui::Button("Connect to serial");
            ImGui::SameLine();
            bool do_disconnect = ImGui::Button("Disconnect");
            ImGui::SameLine();
            if (ImGui::Button("Enumerate devices")) {
                enumerate_devices();
            }

            do_disconnect |= do_connect;

            ImGui::Text("Connection status: ");
            ImGui::SameLine();
            if (m_connection.is_connected()) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
                ImGui::Text("Connected");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                ImGui::Text("Disconnected");
            }
            ImGui::PopStyleColor(1);

            /*if (ImGui::Button("Send ping")) {
                std::array<uint8_t, 32> arr;
                std::iota(arr.begin(), arr.end(), 1);
                m_connection.tx_buffer(arr.data(), arr.size());
            }*/

            if (do_disconnect)
                m_connection.disconnect();

            if (do_connect)
                m_connection.connect(m_inputs.serial_filename);

            ImGui::Separator();

            const auto stats = m_connection.stats;

            const auto conn_health = stats.rx_packets == 0 ? 1.f : static_cast<float>(stats.rx_packets) / static_cast<float>(stats.rx_packets + stats.rx_dropped_packets);
            const auto conn_ratio = stats.rx_dropped_packets == 0 ? std::numeric_limits<float>::infinity() : static_cast<float>(stats.rx_packets) / static_cast<float>(stats.rx_dropped_packets);
            ImGui::Text("Received packets    : %zu", stats.rx_packets);
            ImGui::Text("Dropped packets     : %zu", stats.rx_dropped_packets);
            ImGui::Text("Connection health   : %.2f%% (%.1f:1)", conn_health * 100.f, conn_ratio);
            ImGui::Text("Redundant packets   : %zu", stats.rx_redundant_packets);
            ImGui::Text("Decode errors (COBS): %zu", stats.decode_errors);
            ImGui::Text("CRC errors          : %zu", stats.crc_errors);
            ImGui::Text("Length errors       : %zu", stats.length_errors);

            ImGui::Separator();

            auto& requested_range = m_inputs.requested_range;

            ImGui::PushItemWidth(256);
            ImGui::InputScalarN("Request range", ImGuiDataType_U32, requested_range.data(), 2, NULL, NULL, "%x", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::PopItemWidth();
            ImGui::SameLine();

            bool do_memory_request = ImGui::Button("Make request");
            do_memory_request &= requested_range[1] > requested_range[0];

            if (do_memory_request) {
                uint32_t addr = requested_range[0];
                uint8_t size = std::min<uint32_t>(requested_range[1] - requested_range[0], 255);

                std::array<uint8_t, 4> possible_sizes{8, 4, 2, 1};

                for (const auto req_size: possible_sizes) {
                    while (size > req_size) {
                        m_connection.tx_packet(Unc::MemoryRequestPacket{
                          .address = addr,
                          .size = req_size,
                        });
                        size -= req_size;
                        addr += req_size;
                    }
                }
            }
        }
        ImGui::End();

        ImGui::Begin("packet inspector");
        {
            std::shared_lock out_lock(m_connection.out_packets_mutex);

            auto const& vcs_p = m_connection.out_last_vcs_packet;
            auto const& bms_p = m_connection.out_last_bms_summary_packet;
            auto const& engine_p = m_connection.out_last_engine_packet;
            auto const& local_p = m_connection.out_last_local_observations_packet;
            auto const& batt_p = m_connection.out_last_battery_array_packet;
            auto const& debug_p = m_connection.out_last_debug_metrics_packet;

            ImGui::Text("Last contact at t+n seconds, n: %f", m_connection.last_contact());

            ImGui::Separator();

            /*ImGui::Text("SMPS voltage/current         : %fV %fA", vcs_p.voltage_smps, vcs_p.voltage_smps);
            ImGui::Text("Engine voltage/current       : %fV %fA", vcs_p.voltage_engine, vcs_p.voltage_engine);
            ImGui::Text("Engine driver voltage/current: %fV %fA", vcs_p.voltage_engine_driver, vcs_p.voltage_engine_driver);
            ImGui::Text("BMS voltage/current          : %fV %fA", vcs_p.voltage_bms, vcs_p.voltage_bms);
            ImGui::Text("Telemetry voltage/current    : %fV %fA", vcs_p.voltage_telemetry, vcs_p.voltage_telemetry);
            ImGui::Text("Engine driver temperature    : %f°C", vcs_p.temperature_engine_driver);
            ImGui::Text("SMPS          temperature    : %f°C", vcs_p.temperature_smps);

            ImGui::Separator();*/

            auto const& [min, max, avg] = bms_p.min_max_avg_cell_voltages;
            auto const& [t0, t1, t2, t3, t4] = bms_p.battery_temperatures;
            ImGui::Text("Min/max/avg cell voltages: %f/%f/%f V", min, max, avg);
            ImGui::Text("Sum of cell voltages     : %fV", bms_p.cell_sum);
            ImGui::Text("Battery temperatures     : %.3f°C | %.3f°C | %.3f°C | %.3f°C | %.3f°C", t0, t1, t2, t3, t4);
            ImGui::Text("Spent power              : %fmAh / %fmWh", bms_p.spent_mah, bms_p.spent_mwh);
            ImGui::Text("State of charge          : %f%%", bms_p.soc_percent);
            ImGui::Text("Current                  : %fA", bms_p.current);

            ImGui::Separator();

            ImGui::Text("Speed         : %f km/h", engine_p.speed);
            ImGui::Text("RPM           : %f", engine_p.rpm);
            ImGui::Text("Duty          : %f%%", engine_p.user_duty);
            ImGui::Text("Reported duty : %f%%", engine_p.reported_duty);

            ImGui::Separator();

            ImGui::Text("coordinates        : %f, %f", local_p.gps_coords.first, local_p.gps_coords.second);
            ImGui::Text("acceleration       : %f, %f, %f",
                        local_p.acceleration[0],
                        local_p.acceleration[1],
                        local_p.acceleration[2]);
            ImGui::Text("gyroscope          : %f, %f, %f",
                        local_p.orientation[0],
                        local_p.orientation[1],
                        local_p.orientation[2]);
            // ImGui::Text("temperature        : %f", local_p.temperature);

            ImGui::Separator();

            ImGui::Text("performance metrics: %u, %u, %u",
                        debug_p.performance_report[0],
                        debug_p.performance_report[1],
                        debug_p.performance_report[2]);

            ImGui::Separator();

            for (size_t i = 0; i < 27; i++) {
                const auto voltage = batt_p.voltages[i];

                ImGui::Text("Cell %zu: %f", i, voltage);
            }
        }
        ImGui::End();

        m_window.clear();
        ImGui::SFML::Render(m_window);
        m_window.display();

        std::ignore = m_window.setActive(false);

        do_render();
    }
};

int main() {
    asio::io_context io_ctx{};

    crow::SimpleApp app;

    CROW_ROUTE(app, "/")
    ([](crow::request const&, crow::response& res) {
        res.write("OK");
        res.end();
    });

    CROW_ROUTE(app, "/test").methods("GET"_method, "POST"_method)([](crow::request const&, crow::response& res) {
        res.write("test");
        res.end();
    });

    const auto app_fut = app.port(8083).run_async();

    sf::RenderWindow window(sf::VideoMode({1280, 720}), "asdasd");
    std::ignore = window.setActive(false);
    auto state_ptr = std::make_unique<State>(io_ctx, window);

    const size_t k_workers = 8uz;
    std::vector<std::thread> workers;
    workers.reserve(k_workers);
    for (auto i = 0uz; i < k_workers; i++) {
        workers.push_back(std::thread([&] {
            io_ctx.run();
        }));
    }

    for (auto i = 0uz; i < k_workers; i++)
        workers[i].join();

    state_ptr.reset();

    std::ignore = window.setActive(true);
    window.close();

    app.stop();
    app_fut.wait();
}
