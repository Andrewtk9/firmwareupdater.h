#ifndef _FIRMWAREUPDATER_H
#define _FIRMWAREUPDATER_H

#include <Arduino.h>

class firmwareupdater {

    private:

    public:
        firmwareupdater(const char* check,const char* upload,const char* confirm);
        void begin(unsigned long timing);
        void loop();
};



#endif