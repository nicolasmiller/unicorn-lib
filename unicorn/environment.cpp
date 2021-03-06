#include "unicorn/environment.hpp"
#include "unicorn/string.hpp"
#include <cerrno>
#include <cstdlib>
#include <system_error>

#if defined(PRI_TARGET_UNIX)
    extern char** environ;
#else
    extern wchar_t** _wenviron;
#endif

using namespace Unicorn::Literals;

namespace Unicorn {

    namespace {

        Mutex env_mutex;

        void check_env(const NativeString& name) {
            static const NativeString not_allowed{'\0','='};
            if (name.empty() || name.find_first_of(not_allowed) != npos)
                throw std::invalid_argument("Invalid environment variable name");
        }

        void check_env(const NativeString& name, const NativeString& value) {
            check_env(name);
            if (value.find(NativeCharacter(0)) != npos)
                throw std::invalid_argument("Invalid environment variable value");
        }

        template <typename C>
        basic_string<C> expand_posix_var(const basic_string<C>& src, size_t& ofs, Environment* env) {
            using S = basic_string<C>;
            static constexpr C c_dollar = C('$');
            static constexpr C c_lbrace = C('{');
            static constexpr C c_rbrace = C('}');
            static const S s_dollar = {c_dollar};
            static const S s_dollar_lbrace = {c_dollar, c_lbrace};
            if (src.size() - ofs == 1) {
                ++ofs;
                return s_dollar;
            }
            S name;
            size_t end = ofs;
            if (src[ofs + 1] == c_dollar) {
                ofs += 2;
                return s_dollar;
            } else if (src[ofs + 1] == c_lbrace) {
                end = src.find(c_rbrace, ofs + 2);
                if (end == npos) {
                    ofs += 2;
                    return s_dollar_lbrace;
                }
                name = src.substr(ofs + 2, end - ofs - 2);
                ofs = end + 1;
            } else {
                size_t end = ofs + 1;
                while (end < src.size() && char_is_alphanumeric_w(src[end]))
                    ++end;
                if (end - ofs == 1) {
                    ++ofs;
                    return s_dollar;
                }
                name = src.substr(ofs + 1, end - ofs - 1);
                ofs = end;
            }
            if (env)
                return env->get(name);
            else
                return get_env(name);
        }

        template <typename C>
        basic_string<C> expand_windows_var(const basic_string<C>& src, size_t& ofs, Environment* env) {
            using S = basic_string<C>;
            static constexpr C c_percent = C('%');
            static const S s_percent = {c_percent};
            if (src.size() - ofs == 1) {
                ++ofs;
                return s_percent;
            }
            S name;
            size_t end = ofs;
            if (src[ofs + 1] == c_percent) {
                ofs += 2;
                return s_percent;
            } else {
                end = src.find(c_percent, ofs + 1);
                if (end == npos) {
                    ++ofs;
                    return s_percent;
                }
                name = src.substr(ofs + 1, end - ofs - 1);
                ofs = end + 1;
            }
            if (env)
                return env->get(name);
            else
                return get_env(name);
        }

        template <typename C>
        basic_string<C> do_expand_env(const basic_string<C>& src, uint32_t flags, Environment* env) {
            using S = basic_string<C>;
            static constexpr C c_dollar = C('$');
            static constexpr C c_percent = C('%');
            S delims;
            if (flags & posix_env)
                delims += c_dollar;
            if (flags & windows_env)
                delims += c_percent;
            if (delims.empty() || src.empty())
                return src;
            S dst;
            size_t i = 0;
            while (i < src.size()) {
                size_t j = src.find_first_of(delims, i);
                if (j == npos) {
                    dst.append(src, i, npos);
                    break;
                }
                if (j > i)
                    dst.append(src, i, j - i);
                if (src[j] == c_dollar)
                    dst += expand_posix_var(src, j, env);
                else
                    dst += expand_windows_var(src, j, env);
                i = j;
            }
            return dst;
        }

    }

    #if defined(PRI_TARGET_UNIX)

        string expand_env(const string& src, uint32_t flags) {
            return do_expand_env(src, flags, nullptr);
        }

        string get_env(const string& name) {
            check_env(name);
            MutexLock lock(env_mutex);
            return cstr(getenv(name.data()));
        }

        bool has_env(const string& name) {
            check_env(name);
            MutexLock lock(env_mutex);
            return getenv(name.data()) != nullptr;
        }

        void set_env(const string& name, const string& value) {
            check_env(name, value);
            MutexLock lock(env_mutex);
            if (setenv(name.data(), value.data(), 1) == -1)
                throw std::system_error(errno, std::generic_category(), "setenv()");
        }

        void unset_env(const string& name) {
            check_env(name);
            MutexLock lock(env_mutex);
            if (unsetenv(name.data()) == -1)
                throw std::system_error(errno, std::generic_category(), "unsetenv()");
        }

    #else

        wstring expand_env(const wstring& src, uint32_t flags) {
            return do_expand_env(src, flags, nullptr);
        }

        wstring get_env(const wstring& name) {
            check_env(name);
            MutexLock lock(env_mutex);
            return cstr(_wgetenv(name.data()));
        }

        bool has_env(const wstring& name) {
            check_env(name);
            MutexLock lock(env_mutex);
            return _wgetenv(name.data()) != nullptr;
        }

        void set_env(const wstring& name, const wstring& value) {
            check_env(name, value);
            MutexLock lock(env_mutex);
            auto key_value = name + L'=' + value;
            if (_wputenv(key_value.data()) == -1)
                throw std::system_error(errno, std::generic_category(), "_wputenv()");
        }

        void unset_env(const wstring& name) {
            check_env(name);
            MutexLock lock(env_mutex);
            set_env(name, {});
        }

    #endif

    // Class Environment

    Environment::Environment(bool from_process) {
        if (from_process)
            load();
    }

    Environment::Environment(const Environment& env):
    map(env.map), block(), index() {}

    Environment::Environment(Environment&& env) noexcept:
    map(move(env.map)), block(), index() {
        env.deconstruct();
    }

    Environment& Environment::operator=(const Environment& env) {
        map = env.map;
        deconstruct();
        return *this;
    }

    Environment& Environment::operator=(Environment&& env) noexcept {
        map = move(env.map);
        deconstruct();
        env.deconstruct();
        return *this;
    }

    NativeString Environment::expand(const NativeString& src, uint32_t flags) {
        return do_expand_env(src, flags, this);
    }

    NativeString Environment::get(const NativeString& name) {
        auto it = map.find(name);
        return it == map.end() ? NativeString() : it->second;
    }

    bool Environment::has(const NativeString& name) {
        return map.count(name) != 0;
    }

    void Environment::set(const NativeString& name, const NativeString& value) {
        deconstruct();
        map[name] = value;
    }

    void Environment::unset(const NativeString& name) {
        deconstruct();
        map.erase(name);
    }

    #if defined(PRI_TARGET_WINDOWS)

        u8string Environment::expand(const u8string& src, uint32_t flags) {
            return do_expand_env(src, flags, this);
        }

        u8string Environment::get(const u8string& name) {
            return to_utf8(get(to_wstring(name)));
        }

        bool Environment::has(const u8string& name) {
            return has(to_wstring(name));
        }

        void Environment::set(const u8string& name, const u8string& value) {
            return set(to_wstring(name), to_wstring(value));
        }

        void Environment::unset(const u8string& name) {
            unset(to_wstring(name));
        }

    #endif

    void Environment::load() {
        deconstruct();
        auto ptr =
            #if defined(PRI_TARGET_UNIX)
                environ;
            #else
                _wenviron;
            #endif
        if (ptr == nullptr) {
            map.clear();
            return;
        }
        string_map env;
        NativeString key, value, kv;
        for (; *ptr != nullptr; ++ptr) {
            kv = *ptr;
            size_t cut = kv.find(NativeCharacter('='));
            if (cut == npos) {
                key = kv;
                value.clear();
            } else {
                key = kv.substr(0, cut);
                value = kv.substr(cut + 1, npos);
            }
            env[key] = value;
        }
        map.swap(env);
    }

    void Environment::reconstruct() {
        if (! block.empty())
            return;
        NativeString temp_block;
        vector<size_t> offsets;
        for (auto& kv: map) {
            offsets.push_back(temp_block.size());
            temp_block += kv.first;
            temp_block += PRI_CHAR('=', NativeCharacter);
            temp_block += kv.second;
            temp_block += NativeCharacter(0);
        }
        temp_block += NativeCharacter(0);
        index.resize(offsets.size() + 1);
        block.swap(temp_block);
        for (size_t i = 0; i < offsets.size(); ++i)
            index[i] = &block[0] + offsets[i];
    }

}
