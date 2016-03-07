#include "Canvas.h"
#include "Scene.h"
#include "BasicCamera.h"

#include <algorithm>
#include <cmath>

void Canvas::update(lua_State *L, float delta)
{
    if (m_actorRemoved)
    {
        // TODO: should we iterate through the add queue as well?
        // Iterate through Actors to find any marked for delete
        auto tail = m_actors.begin();
        for (auto end = m_actors.end(), it = tail; it != end; ++it)
        {
            // Remove actor if it is marked for delete, then skip
            if ((*it)->m_canvas != this)
            {
                (*it)->refRemoved(L);
                continue;
            }

            // Shift elements back if we have empty space from removals
            if (tail != it)
                *tail = *it;
            ++tail;
        }

        // If any Actors were removed, clear the end of the list
        // NOTE: since removed Actors can be in the added queue as well, this won't always be true
        if (tail != m_actors.end())
            m_actors.erase(tail, m_actors.end());

        m_actorRemoved = false;
    }

    // Skip the remainder of the update if we are paused
    if (m_paused)
        return;

    // Order of update dispatch doesn't really matter; choose bottom to top
    for (auto& actor : m_actors)
        actor->update(L, delta);

    // Recently added Actors should now be added to the end
    for (auto& actor : m_added)
    {
        // Remove actor if it is marked for delete, then skip
        if (actor->m_canvas != this)
        {
            actor->refRemoved(L);
            continue;
        }

        // Find first actor in canvas with greater layer
        auto it = m_actors.begin();
        for (auto end = m_actors.end(); it != end; ++it)
            if (actor->getLayer() < (*it)->getLayer())
                break;

        // Insert actor before before one with greater layer (insertion sort)
        m_actors.insert(it, actor);
        //m_actors.push_back(actor);
    }
    m_added.clear();
}

void Canvas::render(IRenderer *renderer)
{
    // Don't render anything if Canvas is not visible
    if (!m_visible)
        return;

    m_camera->preRender(renderer);

    // TODO: add alternate structure to iterate in sorted order/down quadtree
    // NOTE: rendering most recently added last (on top)
    auto end = m_actors.end();
    for (auto it = m_actors.begin(); it != end; ++it)
    {
        // Skip Actor if it is marked for removal
        if ((*it)->m_canvas != this)
            continue;

        (*it)->render(renderer);
    }

    m_camera->postRender(renderer);
}

bool Canvas::mouseEvent(lua_State *L, MouseEvent& event)
{
    // Don't allow capturing mouse events when Canvas is inactive
    if (!m_visible || m_paused)
        return false;

    // Convert click into world coordinates
    // TODO: map a ray/point from camera->world space
    float x, y;
    m_camera->mouseToWorld(event, x, y);

    // NOTE: iterate in reverse order of rendering
    auto end = m_actors.rend();
    for (auto it = m_actors.rbegin(); it != end; ++it)
    {
        // Skip Actor if it is marked for removal
        if ((*it)->m_canvas != this)
            continue;

        // Query if click is inside Actor; could be combined with mouseEvent??
        // TODO: Actor should convert ray/point from world->object space
        if (!(*it)->testMouse(x, y))
            continue;

        // TODO: try next actor down, or absorb the click?
        //return (*it)->mouseEvent(L, event.down); // absorb the click
        if ((*it)->mouseEvent(L, event.down))
            return true; // only absorb click if Actor chose to handle it
    }

    return false;
}

void Canvas::resize(lua_State* L, int width, int height)
{
    m_camera->resize(width, height);
}

// =============================================================================
// Lua library functions
// =============================================================================

int Canvas::canvas_init(lua_State *L)
{
    // Push new metatable on the stack
    luaL_newmetatable(L, "Canvas");

    // Push new table to hold member functions
    static const luaL_Reg library[] =
    {
        {"addActor", canvas_addActor},
        {"removeActor", canvas_removeActor},
        {"clear", canvas_clear},
        {"setCenter", canvas_setCenter},
        {"getCollision", canvas_getCollision},
        {"setPaused", canvas_setPaused},
        {"setVisible", canvas_setVisible},
        {nullptr, nullptr}
    };
    luaL_newlib(L, library);

    // Set index of metatable to function table
    lua_pushstring(L, "__index");
    lua_insert(L, -2); // insert "__index" before function library
    lua_settable(L, -3); // metatable.__index = function library

    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, canvas_delete);
    lua_settable(L, -3);

    // Prevent metatable from being accessed through Lua
    lua_pushstring(L, "__metatable");
    lua_pushstring(L, "private");
    lua_settable(L, -3);

    // Push new table to hold global functions
    //luaL_newlib(L, /*luaL_reg*/);
    lua_newtable(L);
    // TODO: add utility functions
    lua_pushstring(L, "create");
    lua_pushcfunction(L, canvas_create);
    lua_settable(L, -3);

    // Assign global function table to global variable
    lua_setglobal(L, "Canvas");

    return 0;
}

int Canvas::canvas_create(lua_State *L)
{
    // Validate arguments
    float w = 20.f, h = 15.f;
    if (lua_istable(L, 1))
    {
        lua_rawgeti(L, 1, 1);
        w = static_cast<float>(luaL_checknumber(L, -1));
        lua_rawgeti(L, 1, 2);
        h = static_cast<float>(luaL_checknumber(L, -1));
        lua_pop(L, 2);
    }

    bool fixed = lua_isboolean(L, 2) && lua_toboolean(L, 2);

    //Scene *scene = reinterpret_cast<Scene*>(lua_touserdata(state, lua_upvalueindex(1)));
    // TODO: probably not best to get Scene off the registry this way; conflict if we have a Scene metatable
    lua_pushstring(L, "Scene");
    lua_gettable(L, LUA_REGISTRYINDEX);
    Scene *scene = reinterpret_cast<Scene*>(lua_touserdata(L, -1));
    luaL_argcheck(L, scene != nullptr, 1, "invalid Scene\n");

    // Create Actor userdata and construct Actor object in the allocated memory
    Canvas *canvas = reinterpret_cast<Canvas*>(lua_newuserdata(L, sizeof(Canvas)));
    new(canvas) Canvas(); // call the constructor on the already allocated block of memory
    canvas->m_camera = ICameraPtr(new BasicCamera(w, h, fixed));

    //printf("created Canvas(%p)\n", canvas);

    luaL_getmetatable(L, "Canvas");
    lua_setmetatable(L, -2); // -1 is metatable, which gets popped by this call

    scene->addCanvas(canvas);

    // add userdata to registry with bare pointer as the key
    // TODO: if we ever remove the Canvas, need to clear this...
    lua_pushlightuserdata(L, canvas);
    lua_pushvalue(L, -2); // alternatively, push table w/ userdata and callbacks (if Canvas needs)
    lua_settable(L, LUA_REGISTRYINDEX);

    // userdata should be on top of Lua stack
    return 1;
}

int Canvas::canvas_delete(lua_State *L)
{
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));
    //printf("deleting Canvas(%p)\n", canvas);

    // Mark each Actor in primary list for removal
    for (auto& actor : canvas->m_actors)
    {
        if (actor->m_canvas == canvas)
            actor->m_canvas = nullptr;

        actor->refRemoved(L);
    }

    // Mark each actor in pending list for removal
    for (auto& actor : canvas->m_added)
    {
        if (actor->m_canvas == canvas)
            actor->m_canvas = nullptr;

        actor->refRemoved(L);
    }

    canvas->~Canvas(); // manually call destructor before Lua calls free()

    return 0;
}

int Canvas::canvas_addActor(lua_State *L)
{
    // Validate function arguments
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));
    Actor *actor = reinterpret_cast<Actor*>(luaL_checkudata(L, 2, Actor::METATABLE));

    // Don't allow adding an Actor that already belongs to another Canvas
    // NOTE: while we could implicitly remove, might be better to throw an error
    //luaL_argcheck(L, (actor->m_canvas == nullptr), 2, "already belongs to a Canvas\n");

    // Check if Actor already added (a previous removal was still pending)
    // NOTE: this will prevent duplicates and faulty ref counts
    auto& actors = canvas->m_actors;
    auto& added = canvas->m_added;
    if (std::find(actors.begin(), actors.end(), actor) == actors.end() &&
        std::find(added.begin(), added.end(), actor) == added.end())
    {
        // Queue up the add; will take affect after the update loop
        canvas->m_added.push_back(actor);
        actor->refAdded(L, 2); // pass index of the full userdata
    }

    // Finally, mark Actor as added to this Canvas
    actor->m_canvas = canvas;

    return 0;
}

int Canvas::canvas_removeActor(lua_State *L)
{
    // Validate function arguments
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));
    Actor *actor = reinterpret_cast<Actor*>(luaL_checkudata(L, 2, Actor::METATABLE));

    // Actor must belong to this Canvas before we can remove it obviously...
    //luaL_argcheck(L, (actor->m_canvas == canvas), 2, "doesn't belong to this Canvas\n");
    bool isOwner = (actor->m_canvas == canvas);

    // Mark Actor for later removal (after we're done iterating)
    if (isOwner)
    {
        canvas->m_actorRemoved = true;
        actor->m_canvas = nullptr;
    }

    lua_pushboolean(L, isOwner);
    return 1;
}

int Canvas::canvas_clear(lua_State* L)
{
    // Validate function arguments
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));

    // Mark each Actor belonging to primary list for removal
    for (auto& actor : canvas->m_actors)
        if (actor->m_canvas == canvas)
            actor->m_canvas = nullptr;

    // Mark each actor belonging to pending list for removal
    for (auto& actor : canvas->m_added)
        if (actor->m_canvas == canvas)
            actor->m_canvas = nullptr;

    // Notify Canvas that Actors are marked for removal
    canvas->m_actorRemoved = true;

    return 0;
}

int Canvas::canvas_setCenter(lua_State* L)
{
    // Validate function arguments
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));

    if (canvas->m_camera)
        canvas->m_camera->setCenter(x, y);

    return 0;
}

int Canvas::canvas_getCollision(lua_State* L)
{
    // Validate function arguments
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));

    // TODO: possibly want to return a list of collisions
    for (auto& actor : canvas->m_actors)
    {
        // Always skip if marked for removal
        if (actor->m_canvas != canvas)
            continue;

        if (actor->testCollision(x, y))
        {
            // Push Actor userdata on the stack
            lua_pushlightuserdata(L, actor);
            lua_rawget(L, LUA_REGISTRYINDEX);
            return 1;
        }
    }

    // NOTE: newly added Actors SHOULD be eligible for collision
    for (auto& actor : canvas->m_added)
    {
        // Always skip if marked for removal
        if (actor->m_canvas != canvas)
            continue;

        if (actor->testCollision(x, y))
        {
            // Push Actor userdata on the stack
            lua_pushlightuserdata(L, actor);
            lua_rawget(L, LUA_REGISTRYINDEX);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

int Canvas::canvas_setPaused(lua_State *L)
{
    // Validate function arguments
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));
    luaL_argcheck(L, lua_isboolean(L, 2), 2, "must be boolean (true to pause)\n");

    canvas->m_paused = lua_toboolean(L, 2);

    return 0;
}

int Canvas::canvas_setVisible(lua_State *L)
{
    // Validate function arguments
    Canvas *canvas = reinterpret_cast<Canvas*>(luaL_checkudata(L, 1, "Canvas"));
    luaL_argcheck(L, lua_isboolean(L, 2), 2, "must be boolean (true for visible)\n");

    canvas->m_visible = lua_toboolean(L, 2);

    return 0;
}
