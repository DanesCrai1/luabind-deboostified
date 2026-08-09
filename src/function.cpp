// Copyright Daniel Wallin 2008. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#define LUABIND_BUILDING

#include <luabind/function.hpp>
#include <luabind/detail/object.hpp>
#include <luabind/make_function.hpp>
#include <luabind/detail/conversion_policies/conversion_policies.hpp>
#include <luabind/detail/object.hpp>
#include <luabind/lua_extensions.hpp>
#include <luabind/da_call_probe.hpp>

#if DA_LUA_CALL_PROBE
#include <algorithm>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// [DA_PORT] Реализация прибора переходов Lua -> движок. Пояснение — в da_call_probe.hpp.
namespace luabind
{
namespace da_probe
{
namespace
{
enum
{
    // Разных имён за одно обновление объекта много не бывает; при переполнении честно скажем.
    MAX_ROWS = 512,
    // Глубина вложенности вызовов движка. Планировщик GOAP зовёт вычислители, те — снова движок.
    MAX_DEPTH = 128
};

struct slot
{
    const char* name;
    unsigned long long self;
    unsigned long long incl;
    unsigned calls;
};

struct frame
{
    const char* name;
    unsigned long long start;
    unsigned long long child;  // общее время вложенных вызовов, вычитается из собственного
};

bool s_on = false;
bool s_overflow = false;

slot s_rows[MAX_ROWS];
int s_row_count = 0;

frame s_stack[MAX_DEPTH];
int s_depth = 0;

unsigned long long s_window_start = 0;
unsigned long long s_window = 0;
unsigned long long s_engine = 0;
unsigned long long s_calls = 0;

// Имя приходит из function_object::name и живёт столько же, сколько сама функция, поэтому
// сравнивать можно по указателю — это заметно дешевле сравнения строк.
int find_or_add(const char* name)
{
    for (int i = 0; i < s_row_count; ++i)
        if (s_rows[i].name == name)
            return i;

    if (s_row_count >= MAX_ROWS)
    {
        s_overflow = true;
        return -1;
    }

    s_rows[s_row_count].name = name;
    s_rows[s_row_count].self = 0;
    s_rows[s_row_count].incl = 0;
    s_rows[s_row_count].calls = 0;
    return s_row_count++;
}
} // namespace

void begin()
{
    s_row_count = 0;
    // Глубину обнуляем ЗДЕСЬ, а не при выходе: lua_error уходит длинным прыжком мимо нашего
    // возврата, и без этого сброса счётчик уползал бы навсегда после первой же ошибки скрипта.
    s_depth = 0;
    s_engine = 0;
    s_calls = 0;
    s_overflow = false;
    s_window_start = __rdtsc();
    s_on = true;
}

void finish()
{
    s_window = __rdtsc() - s_window_start;
    s_on = false;
}

bool active() { return s_on; }

int top(row* out, int max_rows)
{
    int n = s_row_count < max_rows ? s_row_count : max_rows;
    if (n <= 0)
        return 0;

    // Частичной сортировки хватает: наружу уходит только верхушка.
    std::partial_sort(s_rows, s_rows + n, s_rows + s_row_count,
        [](const slot& a, const slot& b) { return a.self > b.self; });

    for (int i = 0; i < n; ++i)
    {
        out[i].name = s_rows[i].name;
        out[i].self = s_rows[i].self;
        out[i].incl = s_rows[i].incl;
        out[i].calls = s_rows[i].calls;
    }
    return n;
}

unsigned long long window_cycles() { return s_window; }
unsigned long long engine_cycles() { return s_engine; }
unsigned long long calls_total() { return s_calls; }
bool overflowed() { return s_overflow; }

namespace
{
inline void enter(const char* name)
{
    if (s_depth >= 0 && s_depth < MAX_DEPTH)
    {
        s_stack[s_depth].name = name;
        s_stack[s_depth].child = 0;
        s_stack[s_depth].start = __rdtsc();
    }
    ++s_depth;
}

inline void leave()
{
    --s_depth;
    if (s_depth < 0)
    {
        s_depth = 0;
        return;
    }
    if (s_depth >= MAX_DEPTH)
        return;

    frame& f = s_stack[s_depth];
    const unsigned long long incl = __rdtsc() - f.start;
    const unsigned long long self = incl > f.child ? incl - f.child : 0;

    // Родителю отдаём ОБЩЕЕ время ветки: иначе его собственное время вобрало бы нашу работу.
    if (s_depth > 0)
        s_stack[s_depth - 1].child += incl;

    const int i = find_or_add(f.name);
    if (i >= 0)
    {
        s_rows[i].self += self;
        s_rows[i].incl += incl;
        ++s_rows[i].calls;
    }

    s_engine += self;
    ++s_calls;
}
} // namespace

} // namespace da_probe
} // namespace luabind
#endif // DA_LUA_CALL_PROBE

namespace luabind {

    bool g_allow_nil_conversion = false;

    LUABIND_API bool is_nil_conversion_allowed()
    {
	    return g_allow_nil_conversion;
    }

    LUABIND_API void allow_nil_conversion(bool allowed)
    {
	    g_allow_nil_conversion = allowed;
    }

	namespace detail {

		namespace {

			int function_destroy(lua_State* L)
			{
				function_object* fn = *(function_object**)lua_touserdata(L, 1);
				luabind_delete(fn);
				return 0;
			}

			void push_function_metatable(lua_State* L)
			{
				lua_pushstring(L, "luabind.function");
				lua_rawget(L, LUA_REGISTRYINDEX);

				if(lua_istable(L, -1))
					return;

				lua_pop(L, 1);

				lua_newtable(L);

				lua_pushstring(L, "__gc");
				lua_pushcclosure(L, &function_destroy, 0);
				lua_rawset(L, -3);

				lua_pushstring(L, "luabind.function");
				lua_pushvalue(L, -2);
				lua_rawset(L, LUA_REGISTRYINDEX);
			}

			// A pointer to this is used as a tag value to identify functions exported
			// by luabind.
			int function_tag = 0;

			// same, but for non-default functions (not from m_default_members)
			int function_tag_ndef = 0;

		} // namespace unnamed

		LUABIND_API bool is_luabind_function(lua_State* L, int index, bool allow_default /*= true*/)
		{
			if(!lua_getupvalue(L, index, 2))
				return false;
			void* tag = lua_touserdata(L, -1);
			bool result = (tag == &function_tag && allow_default) || tag == &function_tag_ndef;
			lua_pop(L, 1);
			return result;
		}

		namespace
		{

			inline bool is_luabind_function(object const& obj)
			{
				obj.push(obj.interpreter());
				bool result = detail::is_luabind_function(obj.interpreter(), -1);
				lua_pop(obj.interpreter(), 1);
				return result;
			}

		} // namespace unnamed

		LUABIND_API void add_overload(
			object const& context, char const* name, object const& fn)
		{
			function_object* f = *touserdata<function_object*>(std::get<1>(getupvalue(fn, 1)));
			f->name = name;

			if(object overloads = context[name])
			{
				if(is_luabind_function(overloads) && is_luabind_function(fn))
				{
					f->next = *touserdata<function_object*>(std::get<1>(getupvalue(overloads, 1)));
					f->keepalive = overloads;
				}
			}

			context[name] = fn;
		}

#if DA_LUA_CALL_PROBE
		namespace
		{
			// [DA_PORT] Единственная точка, через которую проходит вызов ЛЮБОЙ связанной функции.
			//
			// Раньше в замыкание клался сразу impl->entry — шаблонная точка входа, своя на каждую
			// сигнатуру. Обрамить её значило бы тронуть шаблон и пересобрать весь xrGame; вместо
			// этого кладём одну обычную функцию, а точку входа зовём из неё.
			//
			// Верхнее значение замыкания 1 — та же userdata с указателем на function_object, что
			// читает и сама точка входа, поэтому доступ к имени функции бесплатен. Значение 2
			// (метка луабинда) не трогается, и is_luabind_function продолжает работать.
			int da_entry_dispatch(lua_State* L)
			{
				function_object* impl =
					*(function_object**)lua_touserdata(L, lua_upvalueindex(1));

				if (!da_probe::active())
					return impl->entry(L);

				da_probe::enter(impl->name.empty() ? "<безымянная>" : impl->name.c_str());
				const int results = impl->entry(L);
				da_probe::leave();
				return results;
			}
		} // namespace
#endif

		LUABIND_API object make_function_aux(lua_State* L, function_object* impl, bool default_scope /*= false*/)
		{
			void* storage = lua_newuserdata(L, sizeof(function_object*));
			push_function_metatable(L);
			*(function_object**)storage = impl;
			lua_setmetatable(L, -2);

			void* tag = default_scope ? &function_tag : &function_tag_ndef;
			lua_pushlightuserdata(L, tag);
#if DA_LUA_CALL_PROBE
			lua_pushcclosure(L, &da_entry_dispatch, 2);
#else
			lua_pushcclosure(L, impl->entry, 2);
#endif
			stack_pop pop(L, 1);

			return object(from_stack(L, -1));
		}

		void invoke_context::format_error(
			lua_State* L, function_object const* overloads) const
		{
			char const* function_name =
				overloads->name.empty() ? "<unknown>" : overloads->name.c_str();

			int stacksize = lua_gettop(L);

			if(candidate_index == 0)
			{
				// Overloads
				lua_pushstring(L, "No matching overload found, candidates:\n");
				int count = 0;
				for(function_object const* f = overloads; f != 0; f = f->next)
				{
					if(count != 0)
						lua_pushstring(L, "\n");
					f->format_signature(L, function_name);
					++count;
				}
			}
			else
			{
				// Ambiguous
				lua_pushstring(L, "Ambiguous, candidates:\n");
				for(int i = 0; i < candidate_index; ++i)
				{
					if(i != 0)
						lua_pushstring(L, "\n");
					candidates[i]->format_signature(L, function_name);
				}
			}

			// Print all passed arguments and their types
			lua_pushfstring(L, "\nPassed arguments [%d]: ", stacksize); // Args total cnt
			if (stacksize == 0)
				lua_pushstring(L, "<zero arguments>\n");
			else
			{
				for (int _index = 1; _index <= stacksize; _index++)
				{
					if (_index > 1)
						lua_pushstring(L, ", ");

					// Arg Type
					lua_pushstring(L, lua_typename(L, lua_type(L, _index)));
					// Arg Value
					lua_pushstring(L, " (");
					const char* text = lua52L_tolstring(L, _index, NULL); // automatically pushed to stack
					(void)text;
					lua_pushstring(L, ")");
				}
			lua_pushstring(L, "\n");
			}

			lua_concat(L, lua_gettop(L) - stacksize);
		}

	} // namespace detail
} // namespace luabind

