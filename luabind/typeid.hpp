// Copyright Daniel Wallin 2008. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef LUABIND_TYPEID_081227_HPP
# define LUABIND_TYPEID_081227_HPP

# include <typeinfo>
# include <cstring>
# include <luabind/detail/type_traits.hpp>

namespace luabind {

	class type_id
	{
	public:
		type_id()
			: id(&typeid(null_type))
		{}

		type_id(std::type_info const& id)
			: id(&id)
		{}

		bool operator!=(type_id const& other) const
		{
			return *id != *other.id;
		}

		bool operator==(type_id const& other) const
		{
			return *id == *other.id;
		}

		bool operator<(type_id const& other) const
		{
			// [DA_PORT] std::type_info::before() is inconsistent under MinGW x64
			// across DLL boundaries / with LTO. Use name() string comparison for stability.
			const char* a = id->name();
			const char* b = other.id->name();
			if (a == b) return false;
			if (!a) return true;
			if (!b) return false;
			return std::strcmp(a, b) < 0;
		}

		size_t hash_code() const
		{
			return id->hash_code();
		}

		char const* name() const
		{
			return id->name();
		}

	private:
		std::type_info const* id;
	};

} // namespace luabind

#endif // LUABIND_TYPEID_081227_HPP

