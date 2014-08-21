#include <SFML/Graphics.hpp>
#include "LuaHeader.hpp"
#include "LuaConsole.hpp"
#include "LuaConsoleView.hpp"

int main()
{
    sf::RenderWindow app(sf::VideoMode(890u, 520u), "LuaConsole");
    app.setFramerateLimit(30u);
    lua_State * L = luaL_newstate();
    luaL_openlibs(L);
    sf::Font font;
    font.loadFromFile("DejaVuSansMono.ttf");
    lua::LuaConsole console;
    console.setL(L);
    console.view()->setFont(&font);

    while(app.isOpen())
    {
        sf::Event eve;
        while(app.pollEvent(eve))
        {
            if(eve.type == sf::Event::Closed) app.close();
            console.handleEvent(eve);
        }
        app.clear();
        app.draw(console);
        app.display();
    }
    lua_close(L);
}
