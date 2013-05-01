#ifndef TRACE_HH_
#define TRACE_HH_

#include <iostream>
#include <tuple>
#include <boost/format.hpp>
#include <osv/types.h>
#include <align.hh>
#include <sched.hh>
#include <boost/intrusive/list.hpp>
#include <string>

void enable_trace();
void enable_tracepoint(std::string wildcard);

class tracepoint_base;

struct trace_record {
    tracepoint_base* tp;
    sched::thread* thread;
    unsigned cpu;
    union {
        u8 buffer[0];
        long align[0];
    };
};

trace_record* allocate_trace_record(size_t size);

template <class storage_args, class runtime_args>
class assigner_type;

template <class storage_args, class runtime_args,
          typename assigner_type<storage_args, runtime_args>::type assign>
class tracepointv;

template <typename... args>
struct storage_args
{
};

template <typename... args>
struct runtime_args
{
};

template <typename... s_args, typename... r_args>
struct assigner_type<storage_args<s_args...>, runtime_args<r_args...>> {
    typedef std::tuple<s_args...> (*type)(r_args...);
};

template <size_t idx, size_t N, typename... args>
struct tuple_formatter
{
    static boost::format& format(boost::format& fmt, std::tuple<args...> as) {
        typedef tuple_formatter<idx + 1, N, args...> recurse;
        return recurse::format(fmt % std::get<idx>(as), as);
    }
};

template <size_t N, typename... args>
struct tuple_formatter<N, N, args...>
{
    static boost::format& format(boost::format& fmt, std::tuple<args...> as) {
        return fmt;
    }
};

template <typename... args>
inline
boost::format& format_tuple(boost::format& fmt, std::tuple<args...> as)
{
    return tuple_formatter<size_t(0), sizeof...(args), args...>::format(fmt, as);
}

template <typename... args>
inline
boost::format format_tuple(const char* fmt, std::tuple<args...> as)
{
    boost::format format(fmt);
    return format_tuple(format, as);
}

template <typename T>
struct signature_char;

template <>
struct signature_char<char> {
    static const char sig = 'c';
};

template <>
struct signature_char<s8> {
    static const char sig = 'b';
};

template <>
struct signature_char<u8> {
    static const char sig = 'B';
};

template <>
struct signature_char<s16> {
    static const char sig = 'h';
};

template <>
struct signature_char<u16> {
    static const char sig = 'H';
};

template <>
struct signature_char<s32> {
    static const char sig = 'i';
};

template <>
struct signature_char<u32> {
    static const char sig = 'I';
};

template <>
struct signature_char<s64> {
    static const char sig = 'q';
};

template <>
struct signature_char<u64> {
    static const char sig = 'Q';
};

template <>
struct signature_char<bool> {
    static const char sig = '?';
};

template <typename T>
struct signature_char<T*> {
    static const char sig = 'P';
};

template <>
struct signature_char<const char*> {
    static const char sig = 'p';  // "pascal string"
};

template <typename... args>
struct signature_helper;

template <>
struct signature_helper<> {
    static const u64 sig = 0;
};

template <typename arg0, typename... args>
struct signature_helper<arg0, args...> {
    static const u64 sig = signature_char<arg0>::sig
                    | (signature_helper<args...>::sig << 8);
};

template <typename arg>
struct object_serializer {
    void serialize(arg val, void* buffer) { *static_cast<arg*>(buffer) = val; }
    size_t size() { return sizeof(arg); }
    size_t alignment() { return std::min(sizeof(arg), sizeof(long)); } // FIXME: want to use alignof here
};

template <>
struct object_serializer<const char*> {
    static constexpr size_t max_len = 50;
    void serialize(const char* val, void* _buffer) {
        if (!val) {
            val = "<null>";
        }
        auto buffer = static_cast<unsigned char*>(_buffer);
        *buffer = std::min(max_len-1, strlen(val));
        memcpy(buffer + 1, val, *buffer);
    }
    size_t size() { return max_len; }
    size_t alignment() { return 1; }
};

template <size_t idx, size_t N, typename... args>
struct serializer {
    static void write(void* buffer, size_t offset, std::tuple<args...> as) {
        auto arg = std::get<idx>(as);
        object_serializer<decltype(arg)> s;
        offset = align_up(offset, s.alignment());
        s.serialize(arg, buffer + offset);
        return serializer<idx + 1, N, args...>::write(buffer, offset + s.size(), as);
    }
    static size_t size(size_t offset) {
        typedef typename std::tuple_element<idx, std::tuple<args...>>::type argtype;
        object_serializer<argtype> s;
        offset = align_up(offset, s.alignment());
        return serializer<idx + 1, N, args...>::size(offset + s.size());
    }
};

template <size_t N, typename... args>
struct serializer<N, N, args...> {
    static void write(void* buffer, size_t offset, std::tuple<args...> as) {
    }
    static size_t size(size_t offset) {
        return offset;
    }
};

class tracepoint_base {
public:
    explicit tracepoint_base(const char* _name, const char* _format)
        : name(_name), format(_format) {
        tp_list.push_back(*this);
        try_enable();
    }
    ~tracepoint_base() {
        tp_list.erase(tp_list.iterator_to(*this));
    }
    void enable();
    const char* name;
    const char* format;
    u64 sig;
    bool enabled = false;
    typedef boost::intrusive::list_member_hook<> tp_list_link_type;
    tp_list_link_type tp_list_link;
    static boost::intrusive::list<
        tracepoint_base,
        boost::intrusive::member_hook<tracepoint_base,
                                      tp_list_link_type,
                                      &tracepoint_base::tp_list_link>,
        boost::intrusive::constant_time_size<false>
        > tp_list;
private:
    void try_enable();
};

template <typename... s_args,
          typename... r_args,
          typename assigner_type<storage_args<s_args...>, runtime_args<r_args...>>::type assign>
class tracepointv<storage_args<s_args...>, runtime_args<r_args...>, assign>
    : public tracepoint_base
{
public:
    explicit tracepointv(const char* name, const char* format)
        : tracepoint_base(name, format) {
        sig = signature();
    }
    void operator()(r_args... as) {
        trace_slow_path(assign(as...));
    }
    void trace_slow_path(std::tuple<s_args...> as) __attribute__((cold)) {
        if (enabled) {
            sched::preempt_disable();
            auto tr = allocate_trace_record(size());
            tr->tp = this;
            tr->thread = sched::thread::current();
            tr->cpu = -1;
            if (tr->thread) {
                tr->cpu = tr->thread->tcpu()->id;
            }
            serialize(tr->buffer, as);
            sched::preempt_enable();
        }
    }
    void serialize(void* buffer, std::tuple<s_args...> as) {
        return serializer<0, sizeof...(s_args), s_args...>::write(buffer, 0, as);
    }
    size_t size() {
        return serializer<0, sizeof...(s_args), s_args...>::size(0);
    }
    // Python struct style signature H=u16, I=u32, Q=u64 etc, packed into a
    // u64, lsb=first parameter
    u64 signature() const {
        return signature_helper<s_args...>::sig;
    }
};

template <typename... args>
inline
std::tuple<args...> identity_assign(args... as)
{
    return std::make_tuple(as...);
}

template <typename... args>
using tracepoint = tracepointv<storage_args<args...>,
                               runtime_args<args...>,
                               identity_assign<args...>>;


#endif /* TRACE_HH_ */
