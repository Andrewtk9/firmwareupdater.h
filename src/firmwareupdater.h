#ifndef _FIRMWAREUPDATER_H
#define _FIRMWAREUPDATER_H

#include <Arduino.h>
#define DEBUG_PRINTLN(msg) firmUP.println(msg, __FILE__, __LINE__, __FUNCTION__)
#define DEBUG_PRINT(msg) firmUP.print(msg, __FILE__, __LINE__, __FUNCTION__)

class firmwareupdater {

    private:

    public:
        firmwareupdater(const char* site,const char* version);
        void begin(unsigned long timing);
        void loop();
        void print(const String& msg, const char* arquivo, int linha, const char* funcao);
        void println(const String& msg, const char* arquivo, int linha, const char* funcao);
};



#endif