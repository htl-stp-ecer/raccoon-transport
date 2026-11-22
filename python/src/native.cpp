// The Python binding of raccoon::Transport. Everything that decides how frames move
// (rings, reliable delivery, retained channels, statistics) is in the C++ core; this
// file only converts between Python objects and bytes.

#include <pybind11/eval.h>
#include <pybind11/pybind11.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "raccoon/Transport.h"

namespace py = pybind11;
using raccoon::PublishResult;

namespace
{
    py::object g_publish_error;

    [[noreturn]] void throw_publish_error(PublishResult result, const std::string& channel)
    {
        py::object error = g_publish_error(
            std::string("publish on '") + channel + "' failed: " + raccoon::to_string(result),
            result);
        PyErr_SetObject(g_publish_error.ptr(), error.ptr());
        throw py::error_already_set();
    }

    py::dict latency_to_dict(const raccoon::TransportStats::Latency& v)
    {
        py::dict d;
        d["min_us"] = v.min_us;
        d["max_us"] = v.max_us;
        d["avg_us"] = v.avg_us;
        d["p99_us"] = v.p99_us;
        d["count"] = v.count;
        return d;
    }

    py::dict callback_to_dict(const raccoon::TransportStats::Callback& v)
    {
        py::dict d;
        d["min_us"] = v.min_us;
        d["max_us"] = v.max_us;
        d["avg_us"] = v.avg_us;
        d["total_us"] = v.total_us;
        d["count"] = v.count;
        return d;
    }

    py::dict stats_to_dict(const raccoon::TransportStats& stats)
    {
        py::dict d;
        d["latency"] = latency_to_dict(stats.latency);
        d["callback"] = callback_to_dict(stats.callback);
        py::dict spin;
        spin["min_us"] = stats.spin.min_us;
        spin["max_us"] = stats.spin.max_us;
        spin["avg_us"] = stats.spin.avg_us;
        spin["count"] = stats.spin.count;
        spin["active_count"] = stats.spin.active_count;
        spin["idle_count"] = stats.spin.idle_count;
        d["spin"] = spin;
        d["publishes_deduplicated"] = stats.publishes_deduplicated;
        d["reliable_retransmits"] = stats.reliable_retransmits;
        d["reliable_dropped"] = stats.reliable_dropped;
        d["handler_errors"] = stats.handler_errors;
        py::list channels;
        for (const auto& c : stats.channels)
        {
            py::dict entry;
            entry["name"] = c.name;
            entry["deliveries"] = c.deliveries;
            entry["latency"] = latency_to_dict(c.latency);
            entry["callback"] = callback_to_dict(c.callback);
            channels.append(std::move(entry));
        }
        d["channels"] = std::move(channels);
        return d;
    }

    // A Python object that C++ code keeps in a std::function. The function can be
    // destroyed on a thread without the GIL (the transport releases it at shutdown),
    // so the reference is dropped under the GIL.
    using Held = std::shared_ptr<py::object>;

    Held hold(py::object object)
    {
        return Held(new py::object(std::move(object)), [](py::object* p)
        {
            py::gil_scoped_acquire acquire;
            delete p;
        });
    }

    struct PySubscription
    {
        raccoon::Transport::SubscriptionId id{};
    };

    std::int64_t now_us()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // The Python face of raccoon::Transport.
    class PyTransport
    {
    public:
        PyTransport() : transport_(raccoon::Transport::create()) {}

        void publish(const std::string& channel, py::object message, bool deduplicate)
        {
            // Every message starts with a timestamp; stamp it if the caller left it at 0.
            if (py::hasattr(message, "timestamp") && message.attr("timestamp").cast<long long>() == 0)
                message.attr("timestamp") = static_cast<long long>(now_us());
            const std::string type_name = py::str(message.attr("__class__").attr("__name__"));
            const std::string data = py::reinterpret_borrow<py::bytes>(message.attr("encode")());
            send(channel, type_name.c_str(), data, deduplicate);
        }

        void publish_raw(const std::string& channel, const py::bytes& payload, bool deduplicate)
        {
            const std::string data = payload;
            send(channel, nullptr, data, deduplicate);
        }

        PySubscription subscribe(const std::string& channel, py::function handler, py::object message_type)
        {
            // Frames are delivered on the transport's own thread, with the GIL.
            py::object type = message_type;
            std::string type_name;
            if (!type.is_none()) type_name = py::str(type.attr("__name__"));
            py::object decode = py::none();
            if (!type.is_none()) decode = type.attr("decode");
            auto callback = [channel, handler = hold(std::move(handler)), decode = hold(std::move(decode))](
                                const void* data, int len)
            {
                py::gil_scoped_acquire acquire;
                py::bytes bytes(static_cast<const char*>(data), static_cast<std::size_t>(len));
                (*handler)(channel, decode->is_none() ? py::object(bytes) : (*decode)(bytes));
            };
            raccoon::Transport::SubscriptionId id;
            {
                py::gil_scoped_release release;
                std::lock_guard<std::mutex> lk(mu_);
                transport_.start_background_spin();
                id = type_name.empty() ? transport_.subscribe_raw(channel, std::move(callback))
                                       : transport_.subscribe_typed(channel, type_name.c_str(), std::move(callback));
            }
            if (id == 0)
                throw std::runtime_error("cannot subscribe to '" + channel +
                                         "': not in the channel table, wrong message type, or transport closed");
            return PySubscription{id};
        }

        void unsubscribe(const PySubscription& sub)
        {
            py::gil_scoped_release release;
            transport_.unsubscribe(sub.id);
        }

        py::dict get_and_reset_stats()
        {
            raccoon::TransportStats stats;
            {
                py::gil_scoped_release release;
                stats = transport_.get_and_reset_stats();
            }
            return stats_to_dict(stats);
        }

        void set_reliable_event_handler(py::object handler)
        {
            if (handler.is_none())
            {
                transport_.set_reliable_event_handler({});
                return;
            }
            transport_.set_reliable_event_handler(
                [handler = hold(std::move(handler))](raccoon::Transport::ReliableEvent event,
                                                     const std::string& channel,
                                                     std::int64_t key, std::uint32_t attempts)
                {
                    py::gil_scoped_acquire acquire;
                    try
                    {
                        (*handler)(event == raccoon::Transport::ReliableEvent::GaveUp ? "gave_up" : "retransmit",
                                channel, key, attempts);
                    }
                    catch (const py::error_already_set&) {}
                });
        }

        void set_handler_error_handler(py::object handler)
        {
            if (handler.is_none())
            {
                transport_.set_handler_error_handler({});
                return;
            }
            transport_.set_handler_error_handler(
                [handler = hold(std::move(handler))](const std::string& channel, const std::string& what)
                {
                    py::gil_scoped_acquire acquire;
                    try { (*handler)(channel, what); } catch (const py::error_already_set&) {}
                });
        }

        void shutdown()
        {
            // The spin thread needs the GIL to finish a Python handler; do not hold it.
            py::gil_scoped_release release;
            transport_.shutdown();
        }

        bool is_alive() const { return transport_.is_alive(); }

    private:
        void send(const std::string& channel, const char* type_name, const std::string& data, bool deduplicate)
        {
            raccoon::PublishOptions options;
            options.deduplicate = deduplicate;
            PublishResult result;
            {
                py::gil_scoped_release release;
                result = transport_.publish_typed(channel, type_name, data.data(), data.size(), options);
            }
            if (result != PublishResult::Ok) throw_publish_error(result, channel);
        }

        raccoon::Transport transport_;
        std::mutex mu_;
    };
}

PYBIND11_MODULE(_native, m)
{
    m.doc() = "Native part of raccoon_transport: the shared-memory transport.";

    py::enum_<PublishResult>(m, "PublishResult")
        .value("Ok", PublishResult::Ok)
        .value("NotInTable", PublishResult::NotInTable)
        .value("TypeMismatch", PublishResult::TypeMismatch)
        .value("TooBig", PublishResult::TooBig)
        .value("Closed", PublishResult::Closed);

    // PublishError(message, result): raised when a publish does not return Ok.
    py::exec(R"(
class PublishError(RuntimeError):
    """A publish was refused. `result` is the PublishResult."""
    def __init__(self, message, result):
        super().__init__(message)
        self.result = result
)", m.attr("__dict__"));
    g_publish_error = m.attr("PublishError");

    py::class_<PySubscription>(m, "Subscription")
        .def_property_readonly("subscription_id", [](const PySubscription& s) { return s.id; });

    py::class_<PyTransport>(m, "Transport")
        .def(py::init<>())
        .def("publish", &PyTransport::publish, py::arg("channel"), py::arg("message"),
             py::arg("deduplicate") = false)
        .def("publish_raw", &PyTransport::publish_raw, py::arg("channel"), py::arg("payload"),
             py::arg("deduplicate") = false)
        .def("subscribe", &PyTransport::subscribe, py::arg("channel"), py::arg("handler"),
             py::arg("message_type") = py::none())
        .def("unsubscribe", &PyTransport::unsubscribe, py::arg("subscription"))
        .def("get_and_reset_stats", &PyTransport::get_and_reset_stats)
        .def("set_reliable_event_handler", &PyTransport::set_reliable_event_handler, py::arg("handler"))
        .def("set_handler_error_handler", &PyTransport::set_handler_error_handler, py::arg("handler"))
        .def("shutdown", &PyTransport::shutdown)
        .def("is_alive", &PyTransport::is_alive);
}
