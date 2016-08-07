#ifndef __IUSERDATA_HPP__
#define __IUSERDATA_HPP__

#include "Serializer.h"

#include <new>
#include <cassert>
#include "lua.hpp"
// TODO limit to lua_State* in this file so we can remove header
//struct lua_State;

// TODO limit to Serializer ptr in this file so we can remove header
//class Serializer;
//class ObjectRef;

class IUserdata
{
    int m_refCount;

protected:
    IUserdata(): m_refCount(0) {}

    static constexpr const char* const CLASS_NAME = "IUserdata";

public:
    void pushUserdata(lua_State* L);
    // NOTE should be protected? Wrap in unique_ptr-like class?
    void refAdded(lua_State* L, int index);
    void refRemoved(lua_State* L);

    static IUserdata* testInterface(lua_State* L, int index)
    {
        void* ptr = testInterfaceBase(L, index, const_cast<char*>(CLASS_NAME));
        return reinterpret_cast<IUserdata*>(ptr);
    }

    static IUserdata* checkInterface(lua_State* L, int index)
    {
        void* ptr = testInterfaceBase(L, index, const_cast<char*>(CLASS_NAME));
        luaL_argcheck(L, ptr, index, "expected IUserdata subtype");
        return reinterpret_cast<IUserdata*>(ptr);
    }

protected:
    bool pcall(lua_State* L, const char* method, int in, int out);

    // Base cases for TUserdata helper recursion
    static void initInterface(lua_State* L); // TODO rename initHelper or similar?
    static void constructHelper(lua_State* L, IUserdata* ptr);
    static void destroyHelper(lua_State* L, IUserdata* ptr) {}
    static void serializeHelper(lua_State* L, IUserdata* ptr, Serializer* serializer, ObjectRef* ref);

    static void* testInterfaceBase(lua_State* L, int index, void* className);
    static void* upcastHelper(IUserdata* ptr, void* className)//const char* className)
    {
        //if (strcmp(className, CLASS_NAME) == 0)
        if (className == CLASS_NAME)
            return ptr;

        return nullptr;
    }

private:
    static int script_index(lua_State* L);
    static int script_newindex(lua_State* L);
};

template <class T, class B=IUserdata>
class TUserdata : public B
{
protected:
    // NOTE These must be specialized by derived classes
    //static const char* const CLASS_NAME;
    //static const luaL_Reg METHODS[];

public:
    static T* testUserdata(lua_State* L, int index)
    {
        return reinterpret_cast<T*>(luaL_testudata(L, index, T::CLASS_NAME));
    }

    static T* checkUserdata(lua_State* L, int index)
    {
        return reinterpret_cast<T*>(luaL_checkudata(L, index, T::CLASS_NAME));
    }

    static T* testInterface(lua_State* L, int index)
    {
        void* ptr = B::testInterfaceBase(L, index, const_cast<char*>(T::CLASS_NAME));
        return reinterpret_cast<T*>(ptr);
    }

    static T* checkInterface(lua_State* L, int index)
    {
        void* ptr = B::testInterfaceBase(L, index, const_cast<char*>(T::CLASS_NAME));
        // TODO generate a "expected "..T::CLASS_NAME.." subtype" literal
        luaL_argcheck(L, ptr, index, T::CLASS_NAME);
        return reinterpret_cast<T*>(ptr);
    }

    static void initMetatable(lua_State* L);

protected:
    static void initInterface(lua_State* L)
    {
        B::initInterface(L);

        //B::setMethods(L, METHODS);//T::METHODS);
        lua_pushliteral(L, "methods");
        lua_rawget(L, -2);
        assert(lua_type(L, -1) == LUA_TTABLE);
        luaL_setfuncs(L, T::METHODS, 0);
        lua_pop(L, 1);
    }

    void construct(lua_State* L) {} // child inherits no-op
    static void constructHelper(lua_State* L, T* ptr)
    {
        B::constructHelper(L, ptr);
        int top = lua_gettop(L);
        ptr->construct(L); // if T::construct is protected, requires friend class
        assert(top == lua_gettop(L));
    }

    void destroy(lua_State* L) {} // child inherits no-op
    static void destroyHelper(lua_State* L, T* ptr)
    {
        int top = lua_gettop(L);
        ptr->destroy(L); // if T::destroy is protected, requires friend class
        assert(top == lua_gettop(L));
        B::destroyHelper(L, ptr);
    }

    void serialize(lua_State* L, Serializer* serializer, ObjectRef* ref) {} // child inherits no-op
    static void serializeHelper(lua_State* L, T* ptr, Serializer* serializer, ObjectRef* ref)
    {
        B::serializeHelper(L, ptr, serializer, ref);
        int top = lua_gettop(L);
        ptr->serialize(L, serializer, ref); // if T::serialize is protected, requires friend class
        assert(top == lua_gettop(L));
    }

    static void* upcastHelper(T* ptr, void* className)//const char* className)
    {
        //if (strcmp(className, T::CLASS_NAME) == 0)
        if (className == T::CLASS_NAME)
            return ptr;

        // Implicitly static_cast up to parent type
        return B::upcastHelper(ptr, className);
    }

private:
    static int script_create(lua_State* L)
    {
        // Validate constructor arguments
        luaL_checktype(L, 1, LUA_TTABLE);

        // Create userdata with the full size of the object
        T* ptr = reinterpret_cast<T*>(lua_newuserdata(L, sizeof(T)));

        // Get the metatable for this class
        luaL_getmetatable(L, T::CLASS_NAME);
        assert(lua_type(L, -1) == LUA_TTABLE);
        lua_setmetatable(L, -2);

        new(ptr) T();
        constructHelper(L, ptr);

        return 1;
    }

    static int script_destroy(lua_State* L)
    {
        // Validate userdata
        T* ptr = testUserdata(L, 1);
        assert(ptr != nullptr);

        destroyHelper(L, ptr);
        ptr->~T(); // non-virtual

        return 0;
    }

    static int script_serialize(lua_State* L)
    {
        // Validate userdata
        T* ptr = testUserdata(L, 1);
        assert(ptr != nullptr);
        Serializer* serializer = Serializer::checkSerializer(L, 2);

        // Get the object ref and update the constructor name
        ObjectRef* ref = serializer->getObjectRef(ptr);
        assert(ref != nullptr);
        ref->setConstructor(T::CLASS_NAME);

        // Call specialized helper function
        serializeHelper(L, ptr, serializer, ref);

        return 0;
    }

    static int script_upcast(lua_State* L)
    {
        // Validate userdata
        T* ptr = testUserdata(L, 1);
        assert(ptr != nullptr);
        //const char* className = luaL_checkstring(L, 2);
        void* className = lua_touserdata(L, 2);

        void* cast = upcastHelper(ptr, className);
        //printf("upcast %s(%p) to %s(%p)\n", T::CLASS_NAME, ptr, reinterpret_cast<const char*>(className), cast);
        lua_pushlightuserdata(L, cast);

        return 1;
    }
};

template <class T, class B>
void TUserdata<T, B>::initMetatable(lua_State* L)
{
    // TODO the push string/table functions are unsafe if they fail (out of memory error)
    // TODO assert if init is called more than once?

    // === B::createMetatable(L, CLASS_NAME); ===
    // Push new metatable on the stack
    luaL_newmetatable(L, T::CLASS_NAME);

    // Prevent metatable from being accessed directly
    lua_pushliteral(L, "__metatable");
    lua_pushstring(L, T::CLASS_NAME); // return name if getmetatable is called
    lua_rawset(L, -3);


    // TODO replace with setMethods, move newlib/newtable elsewhere
    // Push function table to be used with __index and __newindex
    lua_pushliteral(L, "methods");
    //luaL_newlib(L, METHODS);//T::METHODS);
    lua_newtable(L);
    lua_rawset(L, -3);

    initInterface(L);
    //B::initInterface(L);
    //B::setMethods(L, METHODS);//T::METHODS);


    // === B::finalizeMetatable(L, CLASS_NAME, script_create, script_delete); ===
    // Make sure to call destructor when the object is GC'd
    lua_pushliteral(L, "__gc");
    lua_pushcfunction(L, script_destroy);
    lua_rawset(L, -3);

    // Add serialization functionality to all userdata types
    lua_pushliteral(L, "serialize");
    lua_pushcfunction(L, script_serialize);
    lua_rawset(L, -3);

    // Add upcast converter needed for testInterface
    lua_pushliteral(L, "upcast");
    lua_pushcfunction(L, script_upcast);
    lua_rawset(L, -3);

    // Pop the metatable from the stack
    lua_pop(L, 1);

    // Push constructor as global function with class name
    lua_pushcfunction(L, script_create);
    lua_setglobal(L, T::CLASS_NAME);
}

#endif