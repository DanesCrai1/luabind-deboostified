// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.

#define LUABIND_BUILDING

#include <luabind/detail/object_rep.hpp>
#include <luabind/detail/class_rep.hpp>

#if LUA_VERSION_NUM < 502
# define lua_getuservalue lua_getfenv
# define lua_setuservalue lua_setfenv
#endif

// [DA_PORT] Счётчик создаваемых обёрток по классам. Читается из xrScriptEngine.
extern "C" {
#define DA_UD_SLOTS 512
const void* da_ud_cls[DA_UD_SLOTS];
unsigned long long da_ud_count[DA_UD_SLOTS];
char da_ud_name[DA_UD_SLOTS][64];
unsigned long long da_ud_total = 0;
// Итог отдаём функцией, а не данными: импорт ДАННЫХ из DLL под MinGW работает не везде,
// а ошибка линковки на данных выглядит так же, как на функциях, и уводит не туда.
LUABIND_API unsigned long long da_ud_total_get(void) { return da_ud_total; }

/* [DA_PORT] Откуда обёртки берутся — по СТРОКЕ СКРИПТА.
** Имя класса говорит ЧТО оборачивают, а не КТО просит. Профилировщик по функциям тут слеп на 83%
** (классы мода — userdata luabind, обернуть их методы из Lua нельзя), а здесь виден живой стек Lua.
** lua_getinfo с "Sl" память не выделяет, значит прибор не искажает то, что меряет.
*/
#define DA_UDSITE_SLOTS 1024
static const void* da_udsite_key[DA_UDSITE_SLOTS];
static unsigned long long da_udsite_count[DA_UDSITE_SLOTS];
static char da_udsite_src[DA_UDSITE_SLOTS][80];
static int da_udsite_line[DA_UDSITE_SLOTS];
static unsigned long long da_udsite_total = 0;

static void da_note_site(lua_State* L)
{
	lua_Debug ar;
	int level;
	for (level = 0; level < 6; level++)
	{
		if (!lua_getstack(L, level, &ar)) return;
		if (!lua_getinfo(L, "Sl", &ar)) return;
		if (ar.source && ar.source[0] == '@') break; /* первый настоящий кадр Lua */
	}
	if (!ar.source || ar.source[0] != '@') return;
	{
		size_t h = ((size_t)(void*)ar.source ^ ((size_t)ar.currentline * 2654435761u)) & (DA_UDSITE_SLOTS - 1);
		size_t probe;
		da_udsite_total++;
		for (probe = 0; probe < 32; probe++)
		{
			size_t idx = (h + probe) & (DA_UDSITE_SLOTS - 1);
			if (da_udsite_key[idx] == (const void*)ar.source && da_udsite_line[idx] == ar.currentline)
			{ da_udsite_count[idx]++; return; }
			if (da_udsite_key[idx] == 0)
			{
				size_t i = 0;
				const char* s = ar.source;
				da_udsite_key[idx] = (const void*)ar.source;
				da_udsite_line[idx] = ar.currentline;
				da_udsite_count[idx] = 1;
				for (; i + 1 < sizeof(da_udsite_src[idx]) && s[i]; i++) da_udsite_src[idx][i] = s[i];
				da_udsite_src[idx][i] = 0;
				return;
			}
		}
	}
}

LUABIND_API int da_udsite_get(int index, const char** src, int* line, unsigned long long* count)
{
	int i, seen = 0;
	for (i = 0; i < DA_UDSITE_SLOTS; i++)
		if (da_udsite_key[i])
		{
			if (seen == index)
			{ *src = da_udsite_src[i]; *line = da_udsite_line[i]; *count = da_udsite_count[i]; return 1; }
			seen++;
		}
	return 0;
}

#define DA_DEP_SLOTS 256
static const void* da_dep_ret[DA_DEP_SLOTS];
static unsigned long long da_dep_count[DA_DEP_SLOTS];
static unsigned long long da_dep_total = 0;

LUABIND_API int da_dep_get(int index, const void** ret, unsigned long long* count)
{
	int i, seen = 0;
	for (i = 0; i < DA_DEP_SLOTS; i++)
		if (da_dep_ret[i])
		{
			if (seen == index) { *ret = da_dep_ret[i]; *count = da_dep_count[i]; return 1; }
			seen++;
		}
	return 0;
}

/* [DA_PORT] Учёт РАЗНЫХ указателей среди оборачиваемых. Открытая адресация, при переполнении
** перестаём заводить новые записи и честно печатаем, что счёт неполон.
*/
#define DA_PTR_SLOTS 16384
static const void* da_ptr_set[DA_PTR_SLOTS];
static unsigned long long da_ptr_total = 0;
static unsigned long long da_ptr_distinct = 0;
static unsigned long long da_ptr_overflow = 0;

/* [DA_PORT] Кэш обёрток по указателю на объект.
**
** ЗАЧЕМ. Замер: 61 130 обёрток за 300 кадров на 1861 РАЗНЫЙ объект — по 32.8 обёртки на объект.
** Каждая это 128 байт userdata, и после трёх предыдущих правок именно они составляют 86% всего
** мусора Lua. Кэш убирает 97% из них.
**
** ⛔ ПОЧЕМУ ОПТ-ИН ПО КЛАССУ, А НЕ ДЛЯ ВСЕХ. Ключ здесь — адрес объекта C++. Если объект умрёт, а
** запись останется, освободившийся адрес рано или поздно достанется ДРУГОМУ объекту, и мы отдадим
** в скрипт обёртку от покойника. Это хуже любого мусора. Поэтому кэшируются только классы, чей
** деструктор сам зовёт da_cache_forget: сейчас это единственный game_object (CScriptGameObject
** живёт один на CGameObject, см. m_lua_game_object).
**
** ⛔ ПЕРЕСОЗДАНИЕ СОСТОЯНИЯ. Номера ссылок принадлежат конкретному lua_State, а он пересоздаётся
** на каждой загрузке. Хранить lua_State* и сравнивать НЕЛЬЗЯ: аллокатор охотно отдаст новому
** состоянию тот же адрес. Поэтому движок сам зовёт da_cache_reset — до закрытия состояния (тогда
** записи просто выбрасываются) и после создания нового.
*/
#define DA_CACHE_SLOTS 8192
#define DA_CACHE_NAMES 8

static const char* da_cache_class_names[DA_CACHE_NAMES];
static int da_cache_class_count = 0;

/* class_rep -> разрешён ли кэш. Имя сверяется один раз на класс, дальше сравнение указателей. */
#define DA_CACHE_CLS 128
static const void* da_cache_cls_key[DA_CACHE_CLS];
static int da_cache_cls_ok[DA_CACHE_CLS];

static const void* da_cache_ptr[DA_CACHE_SLOTS];
/* ⚠️ Ключ — ПАРА (класс, указатель), а не один указатель. Пока разрешён один класс, разницы нет,
** но у базового подобъекта адрес совпадает с адресом производного, и на втором разрешённом
** классе кэш начал бы отдавать чужую обёртку. Дефект дешевле закрыть заранее, чем ловить потом. */
static const void* da_cache_cls[DA_CACHE_SLOTS];
static int da_cache_ref[DA_CACHE_SLOTS];
static lua_State* da_cache_state = 0;
static unsigned long long da_cache_hit = 0, da_cache_miss = 0, da_cache_full = 0;

LUABIND_API void da_cache_enable_class(const char* name)
{
	if (da_cache_class_count < DA_CACHE_NAMES) da_cache_class_names[da_cache_class_count++] = name;
}

LUABIND_API void da_cache_reset(lua_State* L)
{
	int i;
	for (i = 0; i < DA_CACHE_SLOTS; i++) { da_cache_ptr[i] = 0; da_cache_cls[i] = 0; da_cache_ref[i] = 0; }
	for (i = 0; i < DA_CACHE_CLS; i++) { da_cache_cls_key[i] = 0; da_cache_cls_ok[i] = 0; }
	da_cache_state = L;
	da_cache_hit = da_cache_miss = da_cache_full = 0;
}

LUABIND_API void da_cache_stat(unsigned long long* hit, unsigned long long* miss,
	unsigned long long* full)
{ *hit = da_cache_hit; *miss = da_cache_miss; *full = da_cache_full; }

static int da_cache_class_allowed(const void* cls, const char* name)
{
	size_t h = (((size_t)cls) >> 4) & (DA_CACHE_CLS - 1);
	size_t probe;
	for (probe = 0; probe < 16; probe++)
	{
		size_t idx = (h + probe) & (DA_CACHE_CLS - 1);
		if (da_cache_cls_key[idx] == cls) return da_cache_cls_ok[idx];
		if (da_cache_cls_key[idx] == 0)
		{
			int ok = 0, i;
			for (i = 0; i < da_cache_class_count; i++)
				if (name && strcmp(name, da_cache_class_names[i]) == 0) { ok = 1; break; }
			da_cache_cls_key[idx] = cls;
			da_cache_cls_ok[idx] = ok;
			return ok;
		}
	}
	return 0;
}

/* Кладёт готовую обёртку на стек и возвращает 1, если она есть. */
LUABIND_API int da_cache_push(lua_State* L, const void* cls, const char* name, const void* p)
{
	size_t h, probe;
	if (!p || !da_cache_state || da_cache_state != L) return 0;
	if (!da_cache_class_allowed(cls, name)) return 0;
	h = (((size_t)p) >> 4) & (DA_CACHE_SLOTS - 1);
	for (probe = 0; probe < 32; probe++)
	{
		size_t idx = (h + probe) & (DA_CACHE_SLOTS - 1);
		if (da_cache_ptr[idx] == p && da_cache_cls[idx] == cls)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, da_cache_ref[idx]);
			if (lua_isuserdata(L, -1)) { da_cache_hit++; return 1; }
			lua_pop(L, 1); /* ссылка протухла — заведём заново */
			da_cache_ptr[idx] = 0;
			return 0;
		}
		if (da_cache_ptr[idx] == 0) return 0;
	}
	return 0;
}

/* Запоминает обёртку с вершины стека. Стек не трогает. */
LUABIND_API void da_cache_store(lua_State* L, const void* cls, const char* name, const void* p)
{
	size_t h, probe;
	if (!p || !da_cache_state || da_cache_state != L) return;
	if (!da_cache_class_allowed(cls, name)) return;
	da_cache_miss++;
	h = (((size_t)p) >> 4) & (DA_CACHE_SLOTS - 1);
	for (probe = 0; probe < 32; probe++)
	{
		size_t idx = (h + probe) & (DA_CACHE_SLOTS - 1);
		if ((da_cache_ptr[idx] == p && da_cache_cls[idx] == cls) || da_cache_ptr[idx] == 0)
		{
			if (da_cache_ptr[idx] == p) luaL_unref(L, LUA_REGISTRYINDEX, da_cache_ref[idx]);
			lua_pushvalue(L, -1);
			da_cache_ref[idx] = luaL_ref(L, LUA_REGISTRYINDEX);
			da_cache_ptr[idx] = p;
			da_cache_cls[idx] = cls;
			return;
		}
	}
	da_cache_full++;
}

LUABIND_API void da_cache_forget(const void* p)
{
	size_t h, probe;
	if (!p) return;
	h = (((size_t)p) >> 4) & (DA_CACHE_SLOTS - 1);
	for (probe = 0; probe < 32; probe++)
	{
		size_t idx = (h + probe) & (DA_CACHE_SLOTS - 1);
		if (da_cache_ptr[idx] == p)
		{
			if (da_cache_state) luaL_unref(da_cache_state, LUA_REGISTRYINDEX, da_cache_ref[idx]);
			da_cache_ptr[idx] = 0;
			da_cache_cls[idx] = 0;
			da_cache_ref[idx] = 0;
			return;
		}
		if (da_cache_ptr[idx] == 0) return;
	}
}

LUABIND_API void da_ptr_note(const void* p)
{
	size_t h = (((size_t)p) >> 4) & (DA_PTR_SLOTS - 1);
	size_t probe;
	da_ptr_total++;
	if (!p) return;
	for (probe = 0; probe < 48; probe++)
	{
		size_t idx = (h + probe) & (DA_PTR_SLOTS - 1);
		if (da_ptr_set[idx] == p) return;
		if (da_ptr_set[idx] == 0) { da_ptr_set[idx] = p; da_ptr_distinct++; return; }
	}
	da_ptr_overflow++;
}

LUABIND_API void da_ptr_stat(unsigned long long* total, unsigned long long* distinct,
	unsigned long long* overflow)
{
	*total = da_ptr_total; *distinct = da_ptr_distinct; *overflow = da_ptr_overflow;
}

LUABIND_API void da_ud_reset(void)
{
	int i;
	for (i = 0; i < DA_UD_SLOTS; i++) { da_ud_cls[i] = 0; da_ud_count[i] = 0; da_ud_name[i][0] = 0; }
	da_ud_total = 0;
	for (i = 0; i < DA_UDSITE_SLOTS; i++) { da_udsite_key[i] = 0; da_udsite_count[i] = 0; da_udsite_line[i] = 0; }
	da_udsite_total = 0;
	for (i = 0; i < DA_DEP_SLOTS; i++) { da_dep_ret[i] = 0; da_dep_count[i] = 0; }
	da_dep_total = 0;
	for (i = 0; i < DA_PTR_SLOTS; i++) da_ptr_set[i] = 0;
	da_ptr_total = da_ptr_distinct = da_ptr_overflow = 0;
	// Счётчики кэша тоже по ОКНУ: сам кэш живёт от загрузки, а его статистика должна быть
	// сопоставима с остальными числами отчёта, иначе читается как несуразица.
	da_cache_hit = da_cache_miss = da_cache_full = 0;
}

LUABIND_API int da_ud_get(int index, const char** name, unsigned long long* count)
{
	int i, seen = 0;
	for (i = 0; i < DA_UD_SLOTS; i++)
		if (da_ud_cls[i])
		{
			if (seen == index) { *name = da_ud_name[i]; *count = da_ud_count[i]; return 1; }
			seen++;
		}
	return 0;
}
} // extern "C"

namespace luabind {
	namespace detail {

		// dest is a function that is called to delete the c++ object this struct holds
		object_rep::object_rep(instance_holder* instance, class_rep* crep)
			: m_instance(instance)
			, m_classrep(crep)
		{
		}

		object_rep::~object_rep()
		{
			if(!m_instance)
				return;
			m_instance->~instance_holder();
			deallocate(m_instance);
		}

		void object_rep::add_dependency(lua_State* L, int index)
		{
			// [DA_PORT] Кто объявляет зависимость. Замер: 50 044 таблицы за 300 кадров, 98.7% всех
			// созданных из C++ — то есть по таблице на КАЖДУЮ обёртку. Политика зависимости
			// разворачивается прямо в переходник биндинга, поэтому адрес возврата и назовёт метод.
			{
				const void* ra = __builtin_return_address(0);
				size_t h = (((size_t)ra) >> 4) & (DA_DEP_SLOTS - 1);
				size_t probe;
				da_dep_total++;
				for (probe = 0; probe < 24; probe++)
				{
					size_t idx = (h + probe) & (DA_DEP_SLOTS - 1);
					if (da_dep_ret[idx] == ra) { da_dep_count[idx]++; break; }
					if (da_dep_ret[idx] == 0) { da_dep_ret[idx] = ra; da_dep_count[idx] = 1; break; }
				}
			}
			if(!m_dependency_ref.is_valid())
			{
				lua_newtable(L);
				m_dependency_ref.set(L);
			}
			m_dependency_ref.get(L);

			lua_pushvalue(L, index);
			lua_pushnumber(L, 0);
			lua_rawset(L, -3);
			lua_pop(L, 1);
		}

		int destroy_instance(lua_State* L)
		{
			object_rep* instance = static_cast<object_rep*>(lua_touserdata(L, 1));

			lua_pushstring(L, "__finalize");
			lua_gettable(L, 1);

			if(lua_isnil(L, -1))
			{
				lua_pop(L, 1);
			}
			else
			{
				lua_pushvalue(L, 1);
				lua_call(L, 1, 0);
			}

			instance->~object_rep();

			lua_pushnil(L);
			lua_setmetatable(L, 1);
			return 0;
		}

		namespace
		{

			int set_instance_value(lua_State* L)
			{
				lua_getuservalue(L, 1);
				lua_pushvalue(L, 2);
				lua_rawget(L, -2);

				if(lua_isnil(L, -1) && lua_getmetatable(L, -2))
				{
					lua_pushvalue(L, 2);
					lua_rawget(L, -2);
					lua_replace(L, -3);
					lua_pop(L, 1);
				}

				if(lua_tocfunction(L, -1) == &property_tag)
				{
					// this member is a property, extract the "set" function and call it.
					lua_getupvalue(L, -1, 2);

					if(lua_isnil(L, -1))
					{
						lua_pushfstring(L, "property '%s' is read only", lua_tostring(L, 2));
						lua_error(L);
					}

					lua_pushvalue(L, 1);
					lua_pushvalue(L, 3);
					lua_call(L, 2, 0);
					return 0;
				}

				lua_pop(L, 1);

				if(!lua_getmetatable(L, 4))
				{
					lua_newtable(L);
					lua_pushvalue(L, -1);
					lua_setuservalue(L, 1);
					lua_pushvalue(L, 4);
					lua_setmetatable(L, -2);
				}
				else
				{
					lua_pop(L, 1);
				}

				lua_pushvalue(L, 2);
				lua_pushvalue(L, 3);
				lua_rawset(L, -3);

				return 0;
			}

			int get_instance_value(lua_State* L)
			{
				lua_getuservalue(L, 1);
				lua_pushvalue(L, 2);
				lua_rawget(L, -2);

				if(lua_isnil(L, -1) && lua_getmetatable(L, -2))
				{
					lua_pushvalue(L, 2);
					lua_rawget(L, -2);
				}

				if(lua_tocfunction(L, -1) == &property_tag)
				{
					// this member is a property, extract the "get" function and call it.
					lua_getupvalue(L, -1, 1);
					lua_pushvalue(L, 1);
					lua_call(L, 1, 1);
				}

				return 1;
			}

			int dispatch_operator(lua_State* L)
			{
				for(int i = 0; i < 2; ++i)
				{
					if(get_instance(L, 1 + i))
					{
						int nargs = lua_gettop(L);

						lua_pushvalue(L, lua_upvalueindex(1));
						lua_gettable(L, 1 + i);

						if(lua_isnil(L, -1))
						{
							lua_pop(L, 1);
							continue;
						}

						lua_insert(L, 1); // move the function to the bottom

						nargs = lua_toboolean(L, lua_upvalueindex(2)) ? 1 : nargs;

						if(lua_toboolean(L, lua_upvalueindex(2))) // remove trailing nil
							lua_remove(L, 3);

						lua_call(L, nargs, 1);
						return 1;
					}
				}

				lua_pop(L, lua_gettop(L));
				lua_pushstring(L, "No such operator defined");
				lua_error(L);

				return 0;
			}

		} // namespace unnamed

		LUABIND_API void push_instance_metatable(lua_State* L)
		{
			lua_newtable(L);

			// This is used as a tag to determine if a userdata is a luabind
			// instance. We use a numeric key and a cclosure for fast comparison.
			lua_pushnumber(L, 1);
			lua_pushcclosure(L, get_instance_value, 0);
			lua_rawset(L, -3);

			lua_pushcclosure(L, destroy_instance, 0);
			lua_setfield(L, -2, "__gc");

			lua_pushcclosure(L, get_instance_value, 0);
			lua_setfield(L, -2, "__index");

			lua_pushcclosure(L, set_instance_value, 0);
			lua_setfield(L, -2, "__newindex");

			for(int op = 0; op < number_of_operators; ++op)
			{
				lua_pushstring(L, get_operator_name(op));
				lua_pushvalue(L, -1);
				lua_pushboolean(L, op == op_unm || op == op_len);
				lua_pushcclosure(L, &dispatch_operator, 2);
				lua_settable(L, -3);
			}
		}

		LUABIND_API object_rep* get_instance(lua_State* L, int index)
		{
			object_rep* result = static_cast<object_rep*>(lua_touserdata(L, index));

			if(!result || !lua_getmetatable(L, index))
				return 0;

			lua_rawgeti(L, -1, 1);

			if(lua_tocfunction(L, -1) != &get_instance_value)
				result = 0;

			lua_pop(L, 2);

			return result;
		}

		LUABIND_API object_rep* push_new_instance(lua_State* L, class_rep* cls)
		{
			// [DA_PORT] Учёт обёрток ПО КЛАССАМ. После того как из мусора Lua убрали C-замыкания
			// (52%), первое место заняли эти userdata: 1005 КБ/с, 290 штук на кадр. Гадать, кого
			// именно оборачивают, не надо — class_rep знает своё имя.
			// Таблица открытой адресацией по указателю class_rep; имя копируется один раз при
			// заведении записи.
			{
				size_t h = (((size_t)(void*)cls) >> 4) & (DA_UD_SLOTS - 1);
				size_t probe;
				da_ud_total++;
				for (probe = 0; probe < 32; probe++)
				{
					size_t idx = (h + probe) & (DA_UD_SLOTS - 1);
					if (da_ud_cls[idx] == (const void*)cls) { da_ud_count[idx]++; break; }
					if (da_ud_cls[idx] == 0)
					{
						const char* n = cls ? cls->name() : "?";
						size_t i = 0;
						da_ud_cls[idx] = (const void*)cls;
						da_ud_count[idx] = 1;
						for (; i + 1 < sizeof(da_ud_name[idx]) && n && n[i]; i++) da_ud_name[idx][i] = n[i];
						da_ud_name[idx][i] = 0;
						break;
					}
				}
			}
			da_note_site(L);
			void* storage = lua_newuserdata(L, sizeof(object_rep));
			object_rep* result = new (storage) object_rep(0, cls);
			cls->get_table(L);
			lua_setuservalue(L, -2);
			lua_rawgeti(L, LUA_REGISTRYINDEX, cls->metatable_ref());
			lua_setmetatable(L, -2);
			return result;
		}

	}
}

