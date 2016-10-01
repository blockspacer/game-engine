#include "Serializer.hpp"

#include "lua.hpp"
#include <list>
#include <algorithm>
#include <cassert>

using namespace std::string_literals;

void ObjectRef::setLiteral(const std::string& table, const std::string& key, const std::string& value)
{
    // Alphabetical search from the end by subtable name
    auto it = std::find_if(m_inlines.rbegin(), m_inlines.rend(),
        [&table](InlineRef& iref) {return table.compare(iref.table) >= 0;});

    // Insert alphabetically after entires with same subtable name
    auto iref = m_inlines.emplace(it.base());
    iref->table = table;
    iref->key = key;
    iref->literal = value;
}

void ObjectRef::setInlineRef(const std::string& table, const std::string& key, ObjectRef* ref)
{
    // Alphabetical search from the end by subtable name
    auto it = std::find_if(m_inlines.rbegin(), m_inlines.rend(),
        [&table](InlineRef& iref) {return table.compare(iref.table) >= 0;});

    // Insert alphabetically after entires with same subtable name
    auto iref = m_inlines.emplace(it.base());
    iref->table = table;
    iref->key = key;
    iref->object = ref;

    // Make sure depth is less than all inline children
    if (m_depth >= ref->m_depth)
        m_depth = ref->m_depth - 1;
}

void ObjectRef::setSetterRef(const std::string& key, const std::string& setter, ObjectRef* ref, FunctionRef* func)
{
    // Push setter reference
    m_setters.emplace_back();
    SetterRef& sref = m_setters.back();
    sref.setter = setter.empty() ? "."s + key : ":"s + setter;
    sref.object = ref;
    sref.function = func;

    // Can't inline objects referenced by name in a setter
    m_inlinable = false;
}

// TODO replace warnings with asserts
void Serializer::populateGlobals(const std::string& prefix, lua_State* L, int index)
{
    assert(lua_type(L, index) == LUA_TTABLE);

    // Hide the table itself (probably an error if it's visible somewhere)
    // NOTE this applies to recursively serialized __index tables as well
    m_globals[lua_topointer(L, index)] = std::string();

    // Iterate over table key/value pairs
    lua_pushnil(L);
    if (index < 0) --index; // adjust relative indices
    while (lua_next(L, index))
    {
        const void* ptr = lua_topointer(L, -1);

        // Skip non-reference types and break cycles
        if (ptr != nullptr || m_globals.find(ptr) != m_globals.end())
        {
            // TODO should we allow non-string keys?
            if (lua_type(L, -2) != LUA_TSTRING)
            {
                fprintf(stderr, "WARNING: global with %s key in %s\n", lua_typename(L, lua_type(L, -2)), prefix.c_str());
                lua_pop(L, 1);
                continue;
            }

            std::string key = prefix + lua_tostring(L, -2);
            m_globals[ptr] = key;

            // TODO recurse userdata (warning on tables?)
            if (lua_type(L, -1) == LUA_TUSERDATA)
            {
                int type = luaL_getmetafield(L, -1, "__index");
                if (type != LUA_TNIL)
                {
                    if (type == LUA_TTABLE)
                    {
                        populateGlobals(key + ".", L, -1);
                    }
                    else
                    {
                        fprintf(stderr, "WARNING: global userdata %s with %s index\n", key.c_str(), lua_typename(L, type));
                    }

                    lua_pop(L, 1);
                }
            }
            else if (lua_type(L, -1) == LUA_TTABLE)
            {
                fprintf(stderr, "WARNING: global table %s is mutable\n", key.c_str());
                // NOTE the table itself can be treated as a const global, but it's children cannot
                //populateGlobals(key + ".", L, -1);
            }
        }

        // Remove value, leaving key on top for next iteration of lua_next
        lua_pop(L, 1);
    }
}

std::string* Serializer::getGlobalName(lua_State* L, int index)
{
    const void* ptr = lua_topointer(L, index);

    if (ptr != nullptr)
    {
        auto it = m_globals.find(ptr);
        if (it != m_globals.end())
            return &it->second;
    }

    return nullptr;
}

// TODO possible to overflow the Lua stack if we have a chain of tables; break this up with lua_call as with userdata?
void Serializer::serializeSubtable(ObjectRef* parent, const std::string& table, lua_State* L, int index)
{
    assert(lua_type(L, index) == LUA_TTABLE);

    if (parent == nullptr)
        parent = &m_root;

    std::string prefix = (parent == &m_root) ? "_G[" : "[";

    // Iterate over table key/value pairs
    lua_pushnil(L);
    if (index < 0) --index; // adjust relative indices
    while (lua_next(L, index))
    {
        // Handle different key types
        int type = lua_type(L, -2);
        switch (type)
        {
        case LUA_TNUMBER:
            lua_pushvalue(L, -2);
            serializeMember(parent, table, prefix + lua_tostring(L, -1) + "]", "", L, -2);
            lua_pop(L, 1);
            break;
        case LUA_TSTRING:
            serializeMember(parent, table, lua_tostring(L, -2), "", L, -1);
            break;
        case LUA_TBOOLEAN:
            serializeMember(parent, table, prefix + (lua_toboolean(L, -2) ? "true]" : "false]"), "", L, -1);
            break;
        // TODO handling these cases may be a little trickier
        case LUA_TFUNCTION:
        case LUA_TTABLE:
        case LUA_TUSERDATA:
            /*{
                // Filter read-only globals (userdata, table, function)
                std::string* global = getGlobalName(L, index);
                if (global != nullptr)
                {
                    if (!global->empty())
                        serializeMember(parent, table, *global, "", L, -1);
                }
                else
                {
                }
            }*/
            //break;
        default:
            fprintf(stderr, "unsupported table key type: %s", lua_typename(L, type));
            break;
        }

        // Remove value, leaving key on top for next iteration of lua_next
        lua_pop(L, 1);
    }
}

void Serializer::serializeMember(ObjectRef* parent, const std::string& table, const std::string& key, const std::string& setter, lua_State* L, int index)
{
    if (parent == nullptr)
        parent = &m_root;

    const bool isRoot = (parent == &m_root);

    // Filter read-only globals (userdata, table, function)
    std::string* global = getGlobalName(L, index);
    if (global != nullptr)
    {
        if (!global->empty())
            parent->setLiteral(table, key, *global);
        return;
    }

    int type = lua_type(L, index);
    switch (type)
    {
    case LUA_TNUMBER:
        lua_pushvalue(L, index);
        parent->setLiteral(table, key, lua_tostring(L, -1));
        lua_pop(L, 1);
        break;
    case LUA_TSTRING:
        parent->setString(table, key, lua_tostring(L, index));
        break;
    case LUA_TBOOLEAN:
        parent->setBoolean(table, key, lua_toboolean(L, index));
        break;
    case LUA_TFUNCTION:
        {
            FunctionRef* ref = serializeFunction(parent->m_depth + 1, L, index);

            // Use a setter unless the function is global
            if (isRoot)
            {
                ref->setGlobalName(key);
                parent->setLiteral(table, key, ref->m_name); // NOTE only need to set this if key != m_name
            }
            else
            {
                parent->setSetterRef(key, setter, nullptr, ref);
            }

            break;
        }
    case LUA_TTABLE:
    case LUA_TUSERDATA:
        {
            ObjectRef* ref = serializeObject(parent->m_depth + 1, true, L, index);

            // Break cycle if object is already on the stack
            if (ref->m_onStack)
            {
                parent->setSetterRef(key, setter, ref, nullptr);
                break;
            }

            if (key.empty())
            {
                assert(!isRoot);
                // Force setter instead of inlining
                parent->setSetterRef(key, setter, ref, nullptr);
                ref->m_inlinable = false;
            }
            else
            {
                // Add ref to parent
                parent->setInlineRef(table, key, ref);

                // Set name if object is global
                if (isRoot)
                    ref->setGlobalName(key);
            }

            break;
        }
    default:
        // TODO light userdata, thread? shouldn't ever encounter these
        fprintf(stderr, "can't set attrib %s: unsupported type %s\n", key.c_str(), lua_typename(L, type));
        break;
    }
}

void Serializer::serializeUpvalue(FunctionRef* parent, lua_State* L, int index)
{
    assert(parent != nullptr);

    // Filter read-only globals (userdata, table, function)
    std::string* global = getGlobalName(L, index);
    if (global != nullptr)
    {
        if (!global->empty())
        {
            // TODO parent->setLiteral(table, key, it->second);
            parent->m_upvalues.emplace_back();
            parent->m_upvalues.back().literal = *global;
            //parent->m_upvalues.back().object = nullptr;
            //parent->m_upvalues.back().function = nullptr;
        }
        return;
    }

    int type = lua_type(L, index);
    switch (type)
    {
    case LUA_TNUMBER:
        lua_pushvalue(L, index);
        parent->m_upvalues.emplace_back();
        parent->m_upvalues.back().literal = lua_tostring(L, -1);
        lua_pop(L, 1);
        break;
    case LUA_TSTRING:
        parent->m_upvalues.emplace_back();
        parent->m_upvalues.back().literal = "\""s + lua_tostring(L, index) + "\"";
        break;
    case LUA_TBOOLEAN:
        parent->m_upvalues.emplace_back();
        parent->m_upvalues.back().literal = lua_toboolean(L, index) ? "true" : "false";
        break;
    case LUA_TNIL:
        parent->m_upvalues.emplace_back();
        parent->m_upvalues.back().literal = "nil";
        break;
    case LUA_TFUNCTION:
        {
            FunctionRef* ref = serializeFunction(parent->m_depth + 1, L, index);

            // TODO move this to FunctionRef::setUpvalue
            // Make sure depth is less than all inline children
            if (parent->m_depth >= ref->m_depth)
                parent->m_depth = ref->m_depth - 1;

            parent->m_upvalues.emplace_back();
            parent->m_upvalues.back().function = ref;
            break;
        }
    case LUA_TTABLE:
    case LUA_TUSERDATA:
        parent->m_upvalues.emplace_back();
        parent->m_upvalues.back().object = serializeObject(parent->m_depth + 1, false, L, index);
        break;
    default:
        // TODO light userdata, thread? shouldn't ever encounter these
        fprintf(stderr, "can't set upvalue: unsupported type %s\n", lua_typename(L, type));
        return;
    }
}

void Serializer::serializeSetter(const std::string& setter, lua_State* L, std::initializer_list<int> list)
{
    // Push a new ref at the back of the vector
    m_setters.emplace_back();
    SetterRef& ref = m_setters.back();
    auto& args = ref.args;
    ref.setter = setter;

    for (auto& index : list)
    {
        // Push empty argument on the vector
        args.emplace_back();

        std::string* global = getGlobalName(L, index);
        if (global != nullptr)
        {
            // TODO will raise an error in print if global is restricted/empty
            args.back().literal = *global;
        }
        else
        {
            int type = lua_type(L, index);
            switch (type)
            {
            case LUA_TNUMBER:
                lua_pushvalue(L, index);
                args.back().literal = lua_tostring(L, -1);
                lua_pop(L, 1);
                break;
            case LUA_TSTRING:
                args.back().literal = "\""s + lua_tostring(L, index) + "\"";
                break;
            case LUA_TBOOLEAN:
                args.back().literal = lua_toboolean(L, index) ? "true" : "false";
                break;
            case LUA_TNIL:
                args.back().literal = "nil";
                break;
            case LUA_TFUNCTION:
                args.back().function = serializeFunction(0, L, index);
                break;
            case LUA_TTABLE:
            case LUA_TUSERDATA:
                args.back().object = serializeObject(0, false, L, index);
                break;
            default:
                // TODO light userdata, thread? shouldn't ever encounter these
                fprintf(stderr, "can't set argument: unsupported type %s\n", lua_typename(L, type));
                break;
            }
        }
    }
}

ObjectRef* Serializer::serializeObject(int depth, bool inlinable, lua_State* L, int index)
{
    // Get userdata pointer
    const void* ptr = lua_topointer(L, index);
    assert(ptr != nullptr);

    // If ref already exists, return it
    ObjectRef* ref = getObjectRef(ptr);
    if (ref != nullptr)
    {
        ref->m_inlinable = false;
        return ref;
    }

    // Create a new object ref
    ref = new ObjectRef(depth, inlinable);
    m_objects[ptr] = ObjectRefPtr(ref); // take ownership of bare ptr

    const int type = lua_type(L, index);
    assert(type == LUA_TUSERDATA || type == LUA_TTABLE);

    // Handle userdata and tables separately
    if (type == LUA_TUSERDATA)
    {
        // Get userdata serialize function
        luaL_getmetafield(L, index, "serialize");
        assert(lua_type(L, -1) == LUA_TFUNCTION);

        // Push the userdata after the function
        const int adjIndex = (index < 0) ? index - 1 : index;
        lua_pushvalue(L, adjIndex);
        lua_pushlightuserdata(L, this);

        // Do a regular call; pops function and udata
        lua_call(L, 2, 0);
    }
    else
    {
        serializeSubtable(ref, "", L, index);

        // Serialize the metatable, if it exists
        if (lua_getmetatable(L, index))
        {
            assert(lua_type(L, -1) == LUA_TTABLE);
            const int adjIndex = (index < 0) ? index - 1 : index;
            serializeSetter("setmetatable", L, {adjIndex, -1});
            lua_pop(L, 1);
        }
    }

    // Clear cycle detection flag before returning
    ref->m_onStack = false;

    return ref;
}

int vectorWriter(lua_State *L, const void* p, size_t sz, void* ud)
{
    auto data = reinterpret_cast<std::vector<char>*>(ud);
    auto chunk = reinterpret_cast<const char*>(p);

    //fprintf(stderr, "writing %ld bytes - %ld to ", sz, data->size());
    data->insert(data->end(), chunk, chunk + sz);
    //fprintf(stderr, "%ld\n", data->size());

    return 0;
}

void formatDump(const std::vector<char>& in, std::string& out)
{
    // adapted from private function 'addquoted' in lstrlib.c
    //out = "\""s;
    size_t size = in.size();
    const char* ptr = in.data();
    //for (auto& ch : in)
    while (size--)
    {
        const char ch = *(ptr++);
        const unsigned char uch = (unsigned char)ch;
        const unsigned char unext = size > 0 ? (unsigned char)*ptr : '\0';
        if (ch == '"' || ch == '\\')// || ch == '\n')
        {
            out.push_back('\\');
            out.push_back(ch);
        }
        else if (ch == '\n')
        {
            out.push_back('\\');
            out.push_back('n');
        }
        else if (ch < ' ' || ch > '~')//iscntrl(uch)) // TODO keep UTF-8 codes or escape them?
        {
            char buffer[5];
            if (!isdigit(unext))
                snprintf(buffer, sizeof(buffer), "\\%d", int(uch));
            else
                snprintf(buffer, sizeof(buffer), "\\%03d", int(uch));
            out += buffer;
        }
        else
        {
            out.push_back(ch);
        }
    }
    //out.push_back('"');
}

FunctionRef* Serializer::serializeFunction(int depth, lua_State* L, int index)
{
    assert(lua_type(L, index) == LUA_TFUNCTION);

    // Get function pointer and data store
    const void* ptr = lua_topointer(L, index);
    assert(ptr != nullptr);

    FunctionRef* ref = getFunctionRef(ptr);
    if (ref != nullptr)
        return ref;

    // Create a new function ref
    ref = new FunctionRef(depth);
    m_functions[ptr] = FunctionRefPtr(ref); // take ownership of bare ptr

    //std::string out;
    std::vector<char> data;
    lua_dump(L, vectorWriter, &data, true); // func must be on top of stack
    formatDump(data, ref->m_code);

    for (int i = 1; true; ++i)
    {
        if (!lua_getupvalue(L, index, i))
            break;

        serializeUpvalue(ref, L, -1);

        lua_pop(L, 1);
    }

    // Clear cycle detection flag before returning
    //ref->m_onStack = false;

    return ref;
}

void Serializer::print()
{
    // TODO clean this up!
    // TODO write to buffer/file instead of printf

    int tempIndex = 1;

    // Sort object refs into list by ref depth, descending order
    std::list<ObjectRef*> objectsSorted;
    for (auto& pair : m_objects)
    {
        // Find first index with lower ref depth
        int depth = pair.second->m_depth;
        auto it = std::find_if(objectsSorted.begin(), objectsSorted.end(),
            [depth](ObjectRef* ptr) {return ptr->m_depth < depth;});

        // Insert before index with lower ref depth
        objectsSorted.insert(it, pair.second.get());
    }

    // TODO refactor this with ObjectRef
    // Sort function refs into list by ref depth, descending order
    std::list<FunctionRef*> functionsSorted;
    for (auto& pair : m_functions)
    {
        // Find first index with lower ref depth
        int depth = pair.second->m_depth;
        auto it = std::find_if(functionsSorted.begin(), functionsSorted.end(),
            [depth](FunctionRef* ptr) {return ptr->m_depth < depth;});

        // Insert before index with lower ref depth
        functionsSorted.insert(it, pair.second.get());
    }

    // TODO might make sense to combine ObjectRef and FunctionRef into a common interface here
    for (auto& ref : functionsSorted)
    {
        // TODO names are out of order now, though it doesn't really matter
        if (ref->m_tempName)
            ref->m_name = "_temp"s + std::to_string(tempIndex++);
    }

    for (auto& ref : objectsSorted)
    {
        // Skip inlinable objects
        if (ref->m_inlinable)
            continue;

        // TODO make sure there are no name clashes; can just use any unique name
        if (ref->m_tempName)
            ref->m_name = "_temp"s + std::to_string(tempIndex++);

        // Print object
        printf("%s%s = ", (ref->m_tempName ? "local " : ""), ref->m_name.c_str());
        printObject(ref, 0);
        printf("\n");
    }


    // TODO handle duplicated globals in printInlines
    for (auto& ref : functionsSorted)
    {
        printf("%s%s = ", (ref->m_tempName ? "local " : ""), ref->m_name.c_str());
        //printf("load(\"%s\")\n", ref->m_code.c_str());
        printf("loadClosure(\"%s\"", ref->m_code.c_str());
        for (auto& upvalue : ref->m_upvalues)
        {
            assert((!upvalue.object != !upvalue.function) == upvalue.literal.empty());

            if (upvalue.object)
                printf(", %s", upvalue.object->m_name.c_str());
            else if (upvalue.function)
                printf(", %s", upvalue.function->m_name.c_str());
            else
                printf(", %s", upvalue.literal.c_str());
        }
        printf(")\n");
    }


    // Print global inlines
    printInlines(&m_root, 0);

    // Iterate over setters
    for (auto& parent : objectsSorted)
        printSetters(parent);


    // Iterate over global setters
    for (auto& ref : m_setters)
    {
        printf("%s(", ref.setter.c_str());
        const char* sep = "";
        for (auto& arg : ref.args)
        {
            assert((!arg.object != !arg.function) == arg.literal.empty());

            if (arg.object)
                printf("%s%s", sep, arg.object->m_name.c_str());
            else if (arg.function)
                printf("%s%s", sep, arg.function->m_name.c_str());
            else
                printf("%s%s", sep, arg.literal.c_str());

            sep = ", ";
        }
        printf(")\n");
    }
}

void Serializer::printSetters(ObjectRef* ref)
{
    assert(ref != nullptr);

    for (auto& sref : ref->m_setters)
    {
        // Assert exactly one non-null
        assert(!sref.object != !sref.function);

        const char* name;
        if (sref.object != nullptr)
            name = sref.object->m_name.c_str();
        else
            name = sref.function->m_name.c_str();

        // Use different formatting for setter functions
        if (sref.setter[0] == '.')
            printf("%s%s = %s\n", ref->m_name.c_str(), sref.setter.c_str(), name);
        else //if (sref.setter[0] == ':')
            printf("%s%s(%s)\n", ref->m_name.c_str(), sref.setter.c_str(), name);
    }
}

void Serializer::printInlines(ObjectRef* ref, int indent)
{
    assert(ref != nullptr);
    const bool isRoot = (ref == &m_root);

    bool firstLine = true;
    std::string lastTable;

    for (auto& iref : ref->m_inlines)
    {
        // Skip global objects that have already been printed
        // NOTE will always be true unless more than one global points at the object
        //if (iref.object && isRoot && iref.key == iref.object->m_name)
        // TODO HACK removing redundant function/global inlines here (currently stored as literal)
        if (isRoot && ((iref.object && iref.key == iref.object->m_name) || iref.key == iref.literal))
            continue;

        // Handle change of subtable formatting
        bool tableChanged = (lastTable != iref.table);

        // Close previous subtable
        if (tableChanged && !lastTable.empty())
        {
            indent -= 2;
            printf("\n%*s}", indent, "");
        }

        // Handle end of line formatting
        if (!firstLine)
            printf(isRoot && (lastTable.empty() || tableChanged) ? "\n" : ",\n");

        firstLine = false;

        // Open new subtable
        if (tableChanged)
        {
            printf("%*s%s =\n%*s{\n", indent, "", iref.table.c_str(), indent, "");
            indent += 2;

            lastTable = iref.table;
        }

        // Print the next line
        if (iref.object)
        {
            if (iref.object->m_inlinable)
            {
                // Handle nested objects recursively
                printf("%*s%s = ", indent, "", iref.key.c_str());
                printObject(iref.object, indent);
            }
            else
            {
                printf("%*s%s = %s", indent, "", iref.key.c_str(), iref.object->m_name.c_str());
            }
        }
        else // literal
        {
            printf("%*s%s = %s", indent, "", iref.key.c_str(), iref.literal.c_str());
        }
    }

    // Close the subtable
    if (!lastTable.empty())
    {
        indent -= 2;
        printf("\n%*s}", indent, "");
    }

    if (!firstLine)
        printf("\n");
}

void Serializer::printObject(ObjectRef* ref, int indent)
{
    assert(ref != nullptr);
    printf("%s", ref->m_constructor.c_str());

    if (!ref->m_inlines.empty())
    {
        printf("\n%*s{\n", indent, "");
        printInlines(ref, indent + 2);
        printf("%*s}", indent, "");
    }
    else
    {
        printf("{}");
    }
}
