#include "TiledGraphics.hpp"
#include "Serializer.hpp"
#include "IRenderer.hpp"
#include "TileMap.hpp"
#include "Actor.hpp"

const luaL_Reg TiledGraphics::METHODS[];

void TiledGraphics::render(IRenderer* renderer)
{
    if (!isVisible() || !m_tilemap)
        return;

    renderer->setColor(m_color.r, m_color.g, m_color.b);
    renderer->drawTiles(m_tilemap);
    renderer->drawLines(m_tilemap->m_debug); // TODO remove
}

bool TiledGraphics::testBounds(float x, float y) const
{
    assert(m_actor != nullptr);

    if (!isVisible() || !m_tilemap)
        return false;

    const Transform& transform = m_actor->getTransform();
    x -= transform.getX();
    y -= transform.getY();
    const float width = transform.getW();
    const float height = transform.getH();
    if (x < 0 || x >= width || y < 0 || y >= height)
        return false;

    // TODO check for invisible/masked tiles
    int tileX = x * m_tilemap->getCols() / width;
    int tileY = y * m_tilemap->getRows() / height;
    int index = m_tilemap->getIndex(tileX, tileY);
    return (index != 0);
}

void TiledGraphics::construct(lua_State* L)
{
    lua_pushliteral(L, "tilemap");
    if (lua_rawget(L, 2) != LUA_TNIL)
        setChild(L, m_tilemap, -1);
    lua_pop(L, 1);
}

void TiledGraphics::clone(lua_State* L, TiledGraphics* source)
{
    if (source->m_tilemap)
    {
        // Don't need to clone TileMap; just copy
        //source->m_tilemap->pushClone(L);
        source->m_tilemap->pushUserdata(L);
        setChild(L, m_tilemap, -1);
        lua_pop(L, 1);
    }
}

void TiledGraphics::serialize(lua_State* L, Serializer* serializer, ObjectRef* ref)
{
    serializer->serializeMember(ref, "", "tilemap", "setTilemap", L, m_tilemap);
}

int TiledGraphics::script_getTileMap(lua_State* L)
{
    TiledGraphics* graphics = TiledGraphics::checkUserdata(L, 1);
    return pushMember(L, graphics->m_tilemap);
}

int TiledGraphics::script_setTileMap(lua_State* L)
{
    TiledGraphics* graphics = TiledGraphics::checkUserdata(L, 1);
    graphics->setChild(L, graphics->m_tilemap, 2);
    return 0;
}
