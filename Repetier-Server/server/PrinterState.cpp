/*
 Copyright 2012-2013 Hot-World GmbH & Co. KG
 Author: Roland Littwin (repetier) repetierdev@gmail.com
 Homepage: http://www.repetier.com
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
 http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 
 */

#define _CRT_SECURE_NO_WARNINGS // Disable deprecation warning in VS2005
#define _CRT_SECURE_NO_DEPRECATE 
#define _SCL_SECURE_NO_DEPRECATE 

#include "PrinterState.h"
#include "printer.h"
#include "GCode.h"
#include "PrinterConfigiration.h"

using namespace std;
using namespace boost;
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS // Disable deprecation warning in VS2005
#endif

ExtruderStatus::ExtruderStatus() {
    id = 0;
    tempSet = tempRead = 0;
    output = 0;
    time = 0;
    ePos = eMax = eOffset = ePrinter = 0;
}
void ExtruderStatus::resetPosition() {
    ePos = eMax = eOffset = ePrinter = 0;
}
PrinterState::PrinterState(PrinterPtr p,int minExtruder) {
    printer = p;
    if(printer!=NULL) extruderCount = printer->config->getExtruderCount()+1; else extruderCount = 10;
    if(extruderCount<minExtruder) extruderCount = minExtruder;
    extruder=new ExtruderStatus[extruderCount]; // Always one more in case 0 extruder
    for(int i=0;i<extruderCount;i++)
        extruder[i].id = i;
    reset();
}
    
void PrinterState::reset() {
    mutex::scoped_lock l(mutex);
    for(int i=0;i<extruderCount;i++)
        extruder[i].resetPosition();
    activeExtruder = &extruder[0];
    uploading = false;
    bed.output = 0;
    bed.tempSet = bed.tempRead = 0;
    x = y = z = 0;
    f = 1000;
    lastX = lastY = lastZ = 0;
    xOffset = yOffset = zOffset = eOffset = 0;
    fanOn = false;
    fanVoltage = 0;
    powerOn = true;
    relative = false;
    eRelative = false;
    debugLevel = 6;
    lastline = 0;
    lastZPrint = 0;
    printingTime = 0;
    layer=0;
    hasXHome = hasYHome = hasZHome = false;
    tempMonitor = -1;
    binaryVersion = 0;
    sdcardMounted = true;
    isRepetier = false;
    isMarlin = false;
    speedMultiply = 100;
    flowMultiply = 100;
}

PrinterState::~PrinterState() {
    delete[] extruder;
}
const ExtruderStatus& PrinterState::getExtruder(int extruderId) const {
    if(extruder<0) return *activeExtruder;
    if(extruderId>=extruderCount) extruderId = 0;
    return extruder[extruderId];
}
ExtruderStatus& PrinterState::getExtruder(int extruderId) {
    if(extruderId<0) return *activeExtruder;
    if(extruderId>=extruderCount) extruderId = 0;
    return extruder[extruderId];
}
void PrinterState::analyze(GCode &code)
{
    mutex::scoped_lock l(mutex);
    isG1Move = false;
    if (code.hostCommand)
    {
        string hc = code.hostCommandPart();
        if (hc=="@isathome")
        {
            hasXHome = hasYHome = hasZHome = true;
            x = printer->config->xHome;
            xOffset = 0;
            y = printer->config->yHome;
            yOffset = 0;
            z = printer->config->zHome;
            zOffset = 0;
        }
        return;
    }
    if (code.hasN())
        lastline = code.getN();
        if (uploading && !code.hasM() && code.getM() != 29) return; // ignore upload commands
    if (code.hasG())
    {
        switch (code.getG())
        {
            case 0:
            case 1:
            {
                isG1Move = true;
                x0 = x;
                y0 = y;
                z0 = z;
                activeExtruder->e0 = activeExtruder->ePos;
                eChanged = false;
                if(code.hasF()) f = code.getF();
                    if (relative)
                    {
                        if(code.hasX()) x += code.getX();
                        if(code.hasY()) y += code.getY();
                        if(code.hasZ()) z += code.getZ();
                        if(code.hasE()) {
                            eChanged = code.getE()!=0;
                            activeExtruder->ePos += code.getE();
                            activeExtruder->ePrinter += code.getE();
                        }
                    }
                    else
                    {
                        if (code.hasX()) x = xOffset+code.getX();
                            if (code.hasY()) y = yOffset+code.getY();
                                if (code.hasZ()) {
                                    z = zOffset+code.getZ();
                                }
                        if (code.hasE())
                        {
                            if (eRelative) {
                                eChanged = code.getE()!=0;
                                activeExtruder->ePos += code.getE();
                                activeExtruder->ePrinter += code.getE();
                            } else {
                                eChanged = (eOffset+code.getE())!=activeExtruder->ePos;
                                activeExtruder->ePos = activeExtruder->eOffset + code.getE();
                                activeExtruder->ePrinter = code.getE();
                            }
                        }
                    }
                if (x < printer->config->xMin) { x = printer->config->xMin; hasXHome = false; }
                if (y < printer->config->yMin) { y = printer->config->yMin; hasYHome = false; }
                if (z < printer->config->zMin) { z = printer->config->zMin; hasZHome = false; }
                if (x > printer->config->xMax) { x = printer->config->xMax; hasXHome = false; }
                if (y > printer->config->yMax) { y = printer->config->yMax; hasYHome = false; }
                if (z > printer->config->zMax) { z = printer->config->zMax; hasZHome = false; }
                if (activeExtruder->ePos > activeExtruder->eMax) {
                    activeExtruder->eMax = activeExtruder->ePos;
                    if(z!=lastZPrint) {
                        lastZPrint = z;
                        layer++;
                    }
                }
                double dx = abs(x - lastX);
                double dy = abs(y - lastY);
                double dz = abs(z - lastZ);
                double de = abs(activeExtruder->ePos - activeExtruder->lastE);
                if (dx + dy + dz > 0.001)
                {
                    printingTime += sqrt(dx * dx + dy * dy + dz * dz) * 60.0f / f;
                }
                else printingTime += de * 60.0f / f;
                lastX = x;
                lastY = y;
                lastZ = z;
                activeExtruder->lastE = activeExtruder->ePos;
            }
                break;
            case 28:
            case 161:
            {
                bool homeAll = !(code.hasX() || code.hasY() || code.hasZ());
                if (code.hasX() || homeAll) { xOffset = 0; x = printer->config->xHome; hasXHome = true; }
                if (code.hasY() || homeAll) { yOffset = 0; y = printer->config->yHome; hasYHome = true; }
                if (code.hasZ() || homeAll) { zOffset = 0; z = printer->config->zHome; hasZHome = true; }
                if (code.hasE()) { activeExtruder->eOffset = 0; activeExtruder->ePos = 0; activeExtruder->eMax = 0; }
                break;
            }
            case 162:
            {
                bool homeAll = !(code.hasX() || code.hasY() || code.hasZ());
                if (code.hasX() || homeAll) { xOffset = 0; x = printer->config->xMax; hasXHome = true; }
                if (code.hasY() || homeAll) { yOffset = 0; y = printer->config->yMax; hasYHome = true; }
                if (code.hasZ() || homeAll) { zOffset = 0; z = printer->config->zMax; hasZHome = true; }
                break;
            }
            case 90:
                relative = false;
                break;
            case 91:
                relative = true;
                break;
            case 92:
                if (code.hasX()) { xOffset = x-code.getX(); x = xOffset; }
                if (code.hasY()) { yOffset = y-code.getY(); y = yOffset; }
                if (code.hasZ()) { zOffset = z-code.getZ(); z = zOffset; }
                if (code.hasE()) {
                    activeExtruder->eOffset = activeExtruder->ePos-code.getE();
                    activeExtruder->lastE = activeExtruder->ePos = activeExtruder->eOffset;
                    activeExtruder->ePrinter = code.getE();
                }
                break;
        }
    }
    else if (code.hasM())
    {
        switch (code.getM())
        {
            case 28:
                uploading = true;
                break;
            case 29:
                uploading = false;
                break;
            case 80:
                powerOn = true;
                break;
            case 81:
                powerOn = false;
                break;
            case 82:
                eRelative = false;
                break;
            case 83:
                eRelative = true;
                break;
            case 104:
            case 109:
            {
                int t = -1;
                if(code.hasT()) t = code.getT();
                    if (code.hasS()) getExtruder(t).tempSet = code.getS();
            }
                break;
            case 106:
                fanOn = true;
                if (code.hasS()) fanVoltage = code.getS();
                break;
            case 107:
                fanOn = false;
                break;
            case 110:
                lastline = code.getN();
                break;
            case 111:
                if (code.hasS())
                {
                    debugLevel = code.getS();
                }
                break;
            case 140:
            case 190:
                if (code.hasS()) bed.tempSet = code.getS();
                break;
            case 203: // Temp monitor
                tempMonitor = code.getS();
                break;
        }
    }
    else if (code.hasT())
    {
        activeExtruder = &getExtruder(code.getT());
    }
}
// Extract the value following a identifier ident until the next space or line end.
bool PrinterState::extract(const string& source,const string& ident,string &result)
{
    size_t pos = 0; //source.find(ident);
    size_t len = source.length();
    do
    {
        if(pos>0) pos++;
        pos = source.find(ident, pos);
        if (pos == string::npos) return false;
        if(pos==0) break;
    } while (source[pos-1]!=' ');
    size_t start = pos + ident.length();
    while(start<len && source[pos]==' ') start++;
    size_t end = start;
    while (end < len && source[end] != ' ') end++;
    result = source.substr(start,end-start);
    return true;
}

void PrinterState::analyseResponse(const string &res,uint8_t &rtype) {
    mutex::scoped_lock l(mutex);
    string h;
    char b[100];
    if (extract(res,"FIRMWARE_NAME:",h))
    {
        firmware = h;
        isRepetier = h.find("Repetier")!=string::npos;
        isMarlin = h.find("Marlin")!=string::npos;
        if (extract(res,"FIRMWARE_URL:",h))
        {
            firmwareURL = h;
        }
        if (extract(res,"PROTOCOL_VERSION:",h))
        {
            protocol = h;
        }
        if (extract(res,"MACHINE_TYPE:",h))
        {
            machine = h;
        }
        if (extract(res,"EXTRUDER_COUNT:",h))
        {
            extruderCountSend = atoi(h.c_str());
        }
    }
    if (extract(res,"X:",h))
    {
        double v = atof(h.c_str());
        x = v-xOffset;
    }
    if (extract(res,"Y:",h))
    {
        double v = atof(h.c_str());
        y = v-yOffset;
    }
    if (extract(res,"Z:",h))
    {
        double v = atof(h.c_str());
        z = v-zOffset;
    }
    if (extract(res,"E:",h))
    {
        double v = atof(h.c_str());
        activeExtruder->ePos = v;
    }
    if (extract(res,"T0:",h)) {
        int ecnt = 0;
        do {
            sprintf(b,"T%d:",ecnt);
            if(!extract(res,b,h)) break;
            double t = atof(h.c_str());
            ExtruderStatus &ex = getExtruder(ecnt);
            ex.tempRead = t;
            sprintf(b,"@%d:",ecnt);
            if(extract(res,b,h)) {
                int  eo = atoi(h.c_str());
                if(isMarlin) eo*=2;
                ex.output = eo;
            }
            ecnt++;
        } while(true);
    }
    if (extract(res,"T:",h))
    {
        rtype = 2;
        ExtruderStatus &ex = getExtruder(-1);
        ex.tempRead = atof(h.c_str());
        if (extract(res,"@:",h))
        {
            int eo = atoi(h.c_str());
            if(isMarlin) eo*=2;
            ex.output = eo;
        }
        printer->updateLastTempMutex();
    }
    if (extract(res,"B:",h))
    {
        bed.tempRead = atof(h.c_str());
    }
    if (extract(res,"SpeedMultiply:",h))  {
        rtype = 2;
        speedMultiply = atoi(h.c_str());
    }
    if (extract(res,"FlowMultiply:",h))  {
        rtype = 2;
        flowMultiply = atoi(h.c_str());
    }
    if (extract(res,"TargetExtr0:",h))  {
        rtype = 2;
        ExtruderStatus &ex = getExtruder(0);
        ex.tempSet = atof(h.c_str());
    }
    if (extract(res,"TargetExtr1:",h))  {
        rtype = 2;
        ExtruderStatus &ex = getExtruder(0);
        ex.tempSet = atof(h.c_str());
    }
    if (extract(res,"TargetBed:",h))  {
        rtype = 2;
        bed.tempSet = atof(h.c_str());
    }
    if (extract(res,"Fanspeed:",h))  {
        rtype = 2;
        fanVoltage = atoi(h.c_str());
    }
    if (extract(res,"REPETIER_PROTOCOL:",h))
    {
        binaryVersion = atoi(h.c_str());
    }
}
uint32_t PrinterState::increaseLastline() {
    mutex::scoped_lock l(mutex);
    return ++lastline;
}
uint32_t PrinterState::decreaseLastline() {
    mutex::scoped_lock l(mutex);
    return --lastline;
}
std::string PrinterState::getMoveXCmd(double dx,double f) {
    mutex::scoped_lock l(mutex);
    char buf[100];
    sprintf(buf,"G1 X%.2f F%.0f",relative ? dx : x+dx,f);
    return string(buf);
}
std::string PrinterState::getMoveYCmd(double dy,double f) {
    mutex::scoped_lock l(mutex);
    char buf[100];
    sprintf(buf,"G1 Y%.2f F%.0f",relative ? dy : y+dy,f);
    return string(buf);
    
}
std::string PrinterState::getMoveZCmd(double dz,double f) {
    mutex::scoped_lock l(mutex);
    char buf[100];
    sprintf(buf,"G1 Z%.2f F%.0f",relative ? dz : z+dz,f);
    return string(buf);
    
}
std::string PrinterState::getMoveECmd(double de,double f) {
    mutex::scoped_lock l(mutex);
    char buf[100];
    sprintf(buf,"G1 E%.2f F%.0f",relative || eRelative ? de : activeExtruder->ePrinter+de,f);
    return string(buf);    
}
void PrinterState::setIsathome() {
    mutex::scoped_lock l(mutex);
    hasXHome = true;
    hasYHome = true;
    hasZHome = true;
    x = printer->config->xHome;
    xOffset = 0;
    y = printer->config->yHome;
    yOffset = 0;
    z = printer->config->zHome;
    zOffset = 0;
}

void PrinterState::fillJSONObject(json_spirit::Object &obj) {
    using namespace json_spirit;
    mutex::scoped_lock l(mutex);
    obj.push_back(Pair("activeExtruder",activeExtruder));
    obj.push_back(Pair("x",x));
    obj.push_back(Pair("y",y));
    obj.push_back(Pair("z",z));
    obj.push_back(Pair("fanOn",fanOn));
    obj.push_back(Pair("fanVoltage",fanVoltage));
    obj.push_back(Pair("powerOn",powerOn));
    obj.push_back(Pair("debugLevel",debugLevel));
    obj.push_back(Pair("hasXHome",hasXHome));
    obj.push_back(Pair("hasYHome",hasYHome));
    obj.push_back(Pair("hasZHome",hasZHome));
    obj.push_back(Pair("layer",layer));
    obj.push_back(Pair("sdcardMounted",sdcardMounted));
    obj.push_back(Pair("bedTempSet",bed.tempSet));
    obj.push_back(Pair("bedTempRead",bed.tempRead));
    obj.push_back(Pair("speedMultiply",speedMultiply));
    obj.push_back(Pair("flowMultiply",flowMultiply));
    obj.push_back(Pair("numExtruder",printer->config->getExtruderCount()));
    obj.push_back(Pair("firmware",firmware));
    obj.push_back(Pair("firmwareURL",firmwareURL));
    Array ea;
    for(int i=0;i<printer->config->getExtruderCount();i++) {
        Object e;
        e.push_back(Pair("tempSet",extruder[i].tempSet));
        e.push_back(Pair("tempRead",extruder[i].tempRead));
        e.push_back(Pair("output",extruder[i].output));
        ea.push_back(e);
    }
    obj.push_back(Pair("extruder",ea));
}
void PrinterState::fillJSONObject(json_spirit::mObject &obj) {
    using namespace json_spirit;
    mutex::scoped_lock l(mutex);
    obj["activeExtruder"] = activeExtruder;
    obj["x"] = x;
    obj["y"] = y;
    obj["z"] = z;
    obj["fanOn"] = fanOn;
    obj["fanVoltage"] = fanVoltage;
    obj["powerOn"] = powerOn;
    obj["debugLevel"] = debugLevel;
    obj["hasXHome"] = hasXHome;
    obj["hasYHome"] = hasYHome;
    obj["hasZHome"] = hasZHome;
    obj["layer"] = layer;
    obj["sdcardMounted"] = sdcardMounted;
    obj["bedTempSet"] = bed.tempSet;
    obj["bedTempRead"] = bed.tempRead;
    obj["speedMultiply"] = speedMultiply;
    obj["flowMultiply"] = flowMultiply;
    obj["numExtruder"] = printer->config->getExtruderCount();
    obj["firmware"] = firmware;
    obj["firmwareURL"] = firmwareURL;
    mArray ea;
    for(int i=0;i<printer->config->getExtruderCount();i++) {
        mObject e;
        e["tempSet"] = extruder[i].tempSet;
        e["tempRead"] = extruder[i].tempRead;
        e["output"] = extruder[i].output;
        ea.push_back(e);
    }
    obj["extruder"] = ea;
}
void PrinterState::storePause() {
    pauseX = x-xOffset;
    pauseY = y-yOffset;
    pauseZ = z-zOffset;
    pauseE = activeExtruder->ePos-activeExtruder->eOffset;
    pauseF = f;
    pauseRelative = relative;
}
void PrinterState::injectUnpause() {
    char buf[200];
    printer->injectManualCommand("G90");
    sprintf(buf,"G1 X%.2f Y%.2f F%.0f",pauseX,pauseY,printer->config->xySpeed*60.0);
    printer->injectManualCommand(buf);
    sprintf(buf,"G1 Z%.2f F%.0f",pauseZ,printer->config->zSpeed*60.0);
    printer->injectManualCommand(buf);
    sprintf(buf,"G92 E%.4f",pauseE);
    printer->injectManualCommand(buf);
    if (relative != pauseRelative)
    {
        printer->injectManualCommand(pauseRelative ? "G91" : "G90");
    }
    sprintf(buf,"G1 F%.0f",pauseF); // Reset old speed
    printer->injectManualCommand(buf);
}
