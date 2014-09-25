#include "LuaConsoleModel.hpp"
#include "LuaHeader.hpp"
#include "LuaCompletion.hpp"
#include "LuaConsoleCallbacks.hpp"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <fstream>

namespace lua {

//how many history items and messages(not wide) to keep
const int kHistoryKeptCount = 100;
const int kMessagesKeptCount = 100;

const char * const kHistoryFilename = "luaconsolehistory.txt";
const char * const kInitFilename = "luaconsoleinit.lua";

//this is the best way to do it when we assume 1 console per lua state
static int TheLightKey;

inline static void * getLightKey()
{
    return &TheLightKey;
}

LuaConsoleModel* LuaConsoleModel::getFromRegistry(lua_State* L)
{
    lua_pushlightuserdata(L, getLightKey());
    lua_gettable(L, LUA_REGISTRYINDEX);

    //assume that if present and right type this value really is us since no
    //one has access to our getLightKey easily, might add checking metatable
    //later too (if/when we stick our metatable in registry) for 100% safety
    LuaConsoleModel * ret = nullptr;
    if(lua_type(L, -1) == LUA_TUSERDATA)
        ret = *static_cast<LuaConsoleModel**>(lua_touserdata(L, -1));

    lua_pop(L, 1);
    return ret;
}

LuaConsoleModel::LuaConsoleModel(unsigned options) :
m_dirtyness(1u), //because 0u is what view starts at
m_cur(1),
L(nullptr),
m_callbacks(nullptr),
m_options(options),
m_visible(false),
m_emptyenterrepeat(true)
{
    m_colors[ECC_ERROR] = 0xff0000ff;
    m_colors[ECC_HINT] = 0x00ff00ff;
    m_colors[ECC_CODE] = 0xffff00ff;
    m_colors[ECC_ECHO] = 0xffffffff;

    //read history from file if desired
    if(m_options & ECO_HISTORY)
    {
        std::ifstream file(kHistoryFilename);
        std::string str;
        while(std::getline(file, str) && m_history.size() < kHistoryKeptCount)
        {
            m_history.push_back(str);
        }
    }
    m_hindex = m_history.size();
    setWidth(78u);
}

LuaConsoleModel::~LuaConsoleModel()
{
    //save history to file if desired
    if(m_options & ECO_HISTORY)
    {
        std::ofstream file(kHistoryFilename);
        for(std::size_t i = 0u; i < m_history.size(); ++i)
        {
            file << m_history[i] << std::endl;
        }
    }
}

void LuaConsoleModel::moveCursor(int move)
{
    m_cur += move;
    m_cur = std::max<int>(m_cur, 1);
    m_cur = std::min<int>(m_lastline.size() + 1, m_cur);
    ++m_dirtyness;
}

void LuaConsoleModel::readHistory(int change)
{
    m_hindex += change;
    m_hindex = std::max<int>(m_hindex, 0);
    m_hindex = std::min<int>(m_hindex, m_history.size());

    if(static_cast<std::size_t>(m_hindex) == m_history.size())
    {
        m_lastline.clear();
        moveCursor(kCursorHome);
    }
    else
    {
        m_lastline = m_history[m_hindex];
        moveCursor(kCursorEnd);
    }

    ++m_dirtyness;
}

void LuaConsoleModel::parseLastLine()
{
    assert(L);

    if(m_lastline.size() == 0u && m_emptyenterrepeat && !m_history.empty())
        m_lastline = m_history.back();

    echoColored(m_lastline, m_colors[ECC_CODE]);
    m_history.push_back(m_lastline);

    if(m_history.size() > kHistoryKeptCount) m_history.erase(m_history.begin());

    m_hindex = m_history.size();

    //call before running, in case crash, exit etc.
    if(m_callbacks) m_callbacks->onNewHistoryItem();

    m_buffcmd += m_lastline;
    m_buffcmd += '\n';

    if(luaL_dostring(L, m_buffcmd.c_str()))
    {
        std::size_t len;
        const char * err = lua_tolstring(L, -1, &len);

        if(!lua::incompleteChunkError(err, len))
        {
            m_buffcmd.clear(); //failed normally - clear it
            echoColored(err, m_colors[ECC_ERROR]);
        }

        lua_pop(L, 1);
    }
    else
    {
        m_buffcmd.clear(); //worked & done - clear it
    }

    m_lastline.clear();
    m_cur = 1;
    ++m_dirtyness;
}

void LuaConsoleModel::addChar(char c)
{
    if(c < ' ' || c >= 127 || m_cur >= kInnerWidth || m_lastline.size() + 1u >= kInnerWidth)
        return;

    m_lastline.insert(m_lastline.begin() + m_cur - 1, c);
    ++m_cur;
    ++m_dirtyness;
}

void LuaConsoleModel::backspace()
{
    if(m_cur > 1)
    {
        --m_cur;
        m_lastline.erase(m_cur - 1, 1);
        ++m_dirtyness;
    }
}

void LuaConsoleModel::del()
{
    m_lastline.erase(m_cur - 1, 1);
    ++m_dirtyness;
}

unsigned LuaConsoleModel::getDirtyness() const
{
    return m_dirtyness;
}

//split str on newlines and to fit 'width' length and push to given vector (if not null)
//returns how many messages str was split into

static std::size_t pushWideMessages(const ColoredLine& str, std::vector<ColoredLine>* widemsgs, unsigned width)
{
    std::size_t ret = 0u;
    std::size_t charcount = 0u;
    std::size_t start = 0u;

    //push pieces of str if they go over width or if we encounter a newline
    for(std::size_t i = 0u; i < str.Text.size(); ++i)
    {
        ++charcount;
        if(str.Text[i] == '\n' || charcount >= width)
        {
            if(str.Text[i] == '\n') --charcount;
            if(widemsgs)
            {
                ColoredLine line;
                line.Text = str.Text.substr(start, charcount);
                line.Color = str.Color.substr(start, charcount);
                widemsgs->push_back(line);
            }
            ++ret;
            start = i + 1u;
            charcount = 0u;
        }
    }

    //push last piece if loop didn't
    if(charcount != 0u)
    {
        if(widemsgs)
        {
            ColoredLine line;
            line.Text = str.Text.substr(start, charcount);
            line.Color = str.Color.substr(start, charcount);
            widemsgs->push_back(line);
        }
        ++ret;
    }
    return ret;
}

void LuaConsoleModel::echo(const std::string& str)
{
    echoColored(str, m_colors[ECC_ECHO]);
}

void LuaConsoleModel::echoColored(const std::string& str, unsigned textcolor)
{
    const ColorString color(str.size(), textcolor);
    echoLine(str, color);
}

void LuaConsoleModel::echoLine(const std::string& str, const ColorString& colors)
{
    if(str.empty()) return echoLine(" ", colors); //workaround for a bug??

    ColoredLine line;
    line.Text = str;
    line.Color = colors;
    line.resizeColorToFitText(m_colors[ECC_ECHO]);

    m_msg.push_back(line);

    pushWideMessages(line, &m_widemsg, m_w);

    if(m_msg.size() > kMessagesKeptCount)
    {
        const std::size_t msgs = pushWideMessages(*m_msg.begin(), nullptr, m_w);
        m_msg.erase(m_msg.begin());
        m_widemsg.erase(m_widemsg.begin(), m_widemsg.begin() + msgs);
    }

    ++m_dirtyness;
}

void LuaConsoleModel::setWidth(std::size_t w)
{
    m_w = w;
    m_widemsg.clear();

    for(const ColoredLine& line : m_msg)
        pushWideMessages(line, &m_widemsg, m_w);

    ++m_dirtyness;
}

const std::string& LuaConsoleModel::getWideMsg(int index) const
{
    if(index < 0) index = m_widemsg.size() + index;
    if(index < 0 || static_cast<std::size_t>(index) >= m_widemsg.size()) return m_empty.Text;

    return m_widemsg[index].Text;
}

const ColorString& LuaConsoleModel::getWideColor(int index) const
{
    if(index < 0) index = m_widemsg.size() + index;
    if(index < 0 || static_cast<std::size_t>(index) >= m_widemsg.size()) return m_empty.Color;

    return m_widemsg[index].Color;
}

const std::string& LuaConsoleModel::getLastLine() const
{
    return m_lastline;
}

int LuaConsoleModel::getCurPos() const
{
    return m_cur;
}

static int ConsoleModel_echo(lua_State * L)
{
    LuaConsoleModel * m = *static_cast<LuaConsoleModel**>(lua_touserdata(L, lua_upvalueindex(1)));
    if(m)
        m->echo(luaL_checkstring(L, 1));

    return 0;
}

static int ConsoleModel_gc(lua_State * L)
{
    LuaConsoleModel * m = *static_cast<LuaConsoleModel**>(lua_touserdata(L, 1));
    if(m)
        m->disarmLuaPointer();

    return 0;
}

void LuaConsoleModel::setL(lua_State * L)
{
    //TODO: add support for more L's being linked/using echos at once??
    this->L = L;
    if(L)
    {
        LuaConsoleModel ** ptr = static_cast<LuaConsoleModel**>(lua_newuserdata(L, sizeof (LuaConsoleModel*)));
        (*ptr) = this;
        setLuaPointer(ptr);

        //make and set new metatable with gc in it
        lua_newtable(L); //table
        lua_pushliteral(L, "__gc");
        lua_pushcfunction(L, &ConsoleModel_gc);
        lua_settable(L, -3); //table[gc]=ConsoleModel_gc
        lua_setmetatable(L, -2);

        lua_pushlightuserdata(L, getLightKey());
        lua_pushvalue(L, -2);
        lua_settable(L, LUA_REGISTRYINDEX);

        lua_pushcclosure(L, &ConsoleModel_echo, 1);
        lua_setglobal(L, "echo");

        if(m_options & ECO_INIT)
        {
            if(luaL_dofile(L, kInitFilename) == LUA_OK)
            {
                m_visible = lua_toboolean(L, -1);
            }
            else
            {
                echoColored(lua_tostring(L, -1), m_colors[ECC_ERROR]);
                m_visible = true; //crapped up init is important so show console right away
            }
        }
    }
    else
    {
        m_visible = false;
    }
}

void LuaConsoleModel::tryComplete()
{
    std::vector<std::string> possible; //possible matches
    std::string last;

    prepareHints(L, m_lastline, last);
    const bool normalhints = collectHints(L, possible, last, false);
    const bool hasmetaindex = luaL_getmetafield(L, -1, "__index");
    const bool metahints = hasmetaindex && collectHints(L, possible, last, false);
    if(!(normalhints || metahints))
    {
        //if all else fails, assume we want _any_ completion and use global table
        lua_pushglobaltable(L);
        collectHints(L, possible, last, false);
    }

    lua_settop(L, 0); //pop all trash we put on the stack

    if(possible.size() > 1u)
    {
        std::string msg = possible[0];
        for(std::size_t i = 1u; i < possible.size(); ++i)
        {
            msg += " " + possible[i];
        }
        echoColored(msg, m_colors[ECC_HINT]);

        const std::string commonprefix = commonPrefix(possible);
        m_lastline += commonprefix.substr(last.size());
        ++m_dirtyness;
        moveCursor(kCursorEnd);
    }
    else if(possible.size() == 1)
    {
        //m_lastline.erase(m_lastline.size() - last.size());
        m_lastline += possible[0].substr(last.size());
        ++m_dirtyness;
        moveCursor(kCursorEnd);
    }
}

const std::vector<std::string>& LuaConsoleModel::getHistory() const
{
    return m_history;
}

void LuaConsoleModel::setHistory(const std::vector<std::string>& history)
{
    m_history = history;
    m_hindex = history.size();
}

void LuaConsoleModel::setCallbacks(LuaConsoleCallbacks* callbacks)
{
    m_callbacks = callbacks;
}

void LuaConsoleModel::setVisible(bool visible)
{
    m_visible = visible;
}

bool LuaConsoleModel::isVisible() const
{
    return m_visible;
}

void LuaConsoleModel::toggleVisible()
{
    m_visible = !m_visible;
}

void LuaConsoleModel::setColor(ECONSOLE_COLOR which, unsigned color)
{
    if(which != ECC_COUNT)
    {
        m_colors[which] = color;
    }
}

unsigned LuaConsoleModel::getColor(ECONSOLE_COLOR which) const
{
    if(which == ECC_COUNT)
        return 0xffffffff;

    return m_colors[which];
}

void LuaConsoleModel::setEnterRepeatLast(bool eer)
{
    m_emptyenterrepeat = eer;
}

bool LuaConsoleModel::getEnterRepeatLast() const
{
    return m_emptyenterrepeat;
}

} //lua
