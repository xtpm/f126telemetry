// APEX / native racing telemetry. C++20, Dear ImGui, GLFW and OpenGL 3.3.
// Compact iRacing-reference overlay: flat header, pedals, input bars and a
// rotating wheel. A matching slim strip adds F1 readouts below the graph.
#include "udp.hpp" // Winsock must precede the Windows headers included by GLFW.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using namespace apex;
namespace {
constexpr ImU32 white = IM_COL32(238, 244, 245, 255);
constexpr ImU32 muted = IM_COL32(133, 151, 158, 255);
constexpr ImU32 green = IM_COL32(62, 255, 17, 255);
constexpr ImU32 cyan = IM_COL32(68, 221, 234, 255);
constexpr ImU32 red = IM_COL32(255, 41, 35, 255);
constexpr ImU32 orange = IM_COL32(255, 167, 71, 255);
constexpr ImU32 blue = IM_COL32(82, 167, 255, 255);
constexpr ImU32 edge = IM_COL32(58, 58, 61, 255);
ImU32 alpha(ImU32 color, int a) { return (color & 0x00ffffffu) | (static_cast<ImU32>(a) << 24); }
constexpr float hudWidth = 1120, compactHeight = 280, expandedHeight = 400;
int layoutHeight(int width, bool expanded) {
    return static_cast<int>(std::lround(width * (expanded ? expandedHeight : compactHeight) / hudWidth));
}

struct Options {
    bool live = false, paused = false, topmost = false, borderless = true, hidden = false;
    bool expanded = true, setup = false;
    int season = 2026, port = 20777, frames = 0, width = 560, height = 200;
    float opacity = 0.82f, graphSeconds = 8;
    double demoStart = 38.25;
    std::string screenshot;
    char driver[64] = "Alex Morgan";
};

struct Fonts { ImFont *normal = nullptr, *bold = nullptr, *mono = nullptr; };
ImFont* loadFont(const std::vector<std::string>& paths, float size) {
    for (const auto& p : paths)
        if (std::filesystem::exists(p))
            if (auto* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(p.c_str(), size)) return f;
    ImFontConfig cfg; cfg.SizePixels = size;
    return ImGui::GetIO().Fonts->AddFontDefault(&cfg);
}
Fonts loadFonts() {
    return {
        loadFont({"C:/Windows/Fonts/segoeui.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
                  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}, 18),
        loadFont({"C:/Windows/Fonts/arialbd.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                  "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"}, 24),
        loadFont({"C:/Windows/Fonts/consola.ttf", "/System/Library/Fonts/Menlo.ttc",
                  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"}, 20)
    };
}

struct Canvas {
    ImDrawList* d;
    Fonts f;
    float scale, ox, oy, opacity;
    ImVec2 p(float x, float y) const { return {ox + x * scale, oy + y * scale}; }
    void rect(float x, float y, float w, float h, ImU32 color, float radius = 0) {
        d->AddRectFilled(p(x,y), p(x+w,y+h), color, radius*scale);
    }
    void outline(float x, float y, float w, float h, ImU32 color, float radius = 0, float thickness = 1) {
        d->AddRect(p(x,y), p(x+w,y+h), color, radius*scale, 0, thickness*scale);
    }
    void line(float x, float y, float xx, float yy, ImU32 color, float width = 1) {
        d->AddLine(p(x,y), p(xx,yy), color, width*scale);
    }
    void dot(float x, float y, float r, ImU32 color, bool glow = false) {
        if (glow) for (int i = 3; i > 0; --i) d->AddCircleFilled(p(x,y), (r+i*2)*scale, alpha(color, 10+i*3), 24);
        d->AddCircleFilled(p(x,y), r*scale, color, 24);
    }
    float textWidth(const std::string& s, float size, ImFont* font = nullptr) const {
        return (font ? font : f.normal)->CalcTextSizeA(size*scale, FLT_MAX, 0, s.c_str()).x/scale;
    }
    void text(float x, float y, float size, ImU32 color, const std::string& s, ImFont* font = nullptr) {
        d->AddText(font ? font : f.normal, size*scale, p(x,y), color, s.c_str());
    }
    void right(float x, float y, float size, ImU32 color, const std::string& s, ImFont* font = nullptr) {
        text(x-textWidth(s,size,font),y,size,color,s,font);
    }
    void center(float x, float y, float size, ImU32 color, const std::string& s, ImFont* font = nullptr) {
        text(x-textWidth(s,size,font)/2,y,size,color,s,font);
    }
    void label(float x, float y, const std::string& s, ImU32 color = muted) { text(x,y,12,color,s,f.bold); }
    void fit(float x, float y, float maxWidth, float size, ImU32 color, const std::string& s, ImFont* font = nullptr) {
        const float w = textWidth(s,size,font);
        text(x,y,w > maxWidth ? size*maxWidth/w : size,color,s,font);
    }
    bool button(float x, float y, float w, float h, const char* id, const std::string& title, bool selected = false, ImU32 color = green) {
        ImGui::SetCursorScreenPos(p(x,y));
        const bool clicked = ImGui::InvisibleButton(id,{w*scale,h*scale});
        bool hover = ImGui::IsItemHovered();
        if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (selected || hover) rect(x,y,w,h,selected ? alpha(color,24) : IM_COL32(45,45,48,245),1);
        center(x+w/2,y+(h-14)/2-1,14,hover || selected ? color : white,title,f.bold);
        return clicked;
    }
    void bar(float x, float y, float w, float h, float value, ImU32 color) {
        rect(x,y,w,h,IM_COL32(35,46,50,220),2);
        if (value > 0) rect(x,y,w*clamp01(value),h,color,2);
    }
};

ImU32 tyreColor(float t) { return t < 80 ? blue : t <= 103 ? green : t <= 110 ? orange : red; }
const char* compoundName(int compound) {
    switch (compound) { case 16: return "SOFT"; case 17: return "MEDIUM"; case 18: return "HARD";
        case 7: return "INTER"; case 8: return "WET"; default: return "---"; }
}
std::string gearText(const Telemetry& t) {
    return !t.hasTelemetry ? "-" : t.gear == 0 ? "N" : t.gear < 0 ? "R" : std::to_string(t.gear);
}

// Local vector geometry keeps the wheel sharp and self contained. Rotate only
// its newly emitted vertices; the circular backing and other HUD text stay put.
void steeringWheel(Canvas& c, const Telemetry& t) {
    constexpr float cx = 1000, cy = 157;
    c.dot(cx,cy,109,IM_COL32(4,4,5,static_cast<int>(c.opacity*255)));
    c.d->AddCircle(c.p(cx,cy),108*c.scale,IM_COL32(38,39,42,210),80,c.scale);
    const int first = c.d->VtxBuffer.Size;
    auto poly = [&](std::initializer_list<ImVec2> points, ImU32 color, bool stroke=false) {
        for (auto p:points) c.d->PathLineTo(c.p(cx+p.x,cy+p.y));
        if (stroke) c.d->PathStroke(color,ImDrawFlags_Closed,1.2f*c.scale);
        else c.d->PathFillConcave(color);
    };
    // Grips, central carbon plate, and two inset hand openings.
    const auto shell = {ImVec2{-89,-48},ImVec2{-66,-65},ImVec2{-43,-56},ImVec2{-28,-43},ImVec2{28,-43},
        ImVec2{43,-56},ImVec2{66,-65},ImVec2{89,-48},ImVec2{96,-10},ImVec2{87,47},
        ImVec2{65,65},ImVec2{39,51},ImVec2{25,36},ImVec2{-25,36},ImVec2{-39,51},
        ImVec2{-65,65},ImVec2{-87,47},ImVec2{-96,-10}};
    poly(shell,IM_COL32(14,15,17,255)); poly(shell,IM_COL32(79,82,89,255),true);
    poly({{-72,-49},{-53,-47},{-54,-17},{-59,17},{-48,43},{-65,49},{-76,29},{-82,-7}},IM_COL32(28,29,32,255));
    poly({{72,-49},{53,-47},{54,-17},{59,17},{48,43},{65,49},{76,29},{82,-7}},IM_COL32(28,29,32,255));
    poly({{-58,-29},{-38,-32},{-30,30},{-43,39},{-48,6}},IM_COL32(3,4,5,255));
    poly({{58,-29},{38,-32},{30,30},{43,39},{48,6}},IM_COL32(3,4,5,255));
    poly({{-44,-39},{44,-39},{49,23},{30,43},{-30,43},{-49,23}},IM_COL32(29,32,36,255));
    poly({{-44,-39},{44,-39},{49,23},{30,43},{-30,43},{-49,23}},IM_COL32(64,69,76,255),true);
    // Fine carbon texture is kept on the bridge, with highlights along the grips.
    for (int i=-36; i<36; i+=6)
        for (int j=-29; j<32; j+=6) {
            c.line(cx+i,cy+j,cx+i+3,cy+j+3,IM_COL32(46,50,56,150));
            c.line(cx+i+3,cy+j,cx+i+5,cy+j-2,IM_COL32(12,14,17,170));
        }
    for (int side : {-1,1}) {
        c.line(cx+side*87.0f,cy-30,cx+side*88.0f,cy+7,IM_COL32(92,96,103,185),2);
        for (int j=0;j<6;++j) c.line(cx+side*78.0f,cy-24+j*9.0f,cx+side*85.0f,cy-22+j*9.0f,IM_COL32(11,12,14,255),2);
    }
    // Mini display and actual RPM LEDs remain attached to the rotating wheel.
    c.rect(cx-27,cy-29,54,40,IM_COL32(3,7,10,255),3);
    c.outline(cx-27,cy-29,54,40,IM_COL32(98,108,113,255),3);
    c.center(cx,cy-25,27,white,gearText(t),c.f.bold);
    c.rect(cx-23,cy+5,46,3,alpha(green,80));
    for (int i=0;i<9;++i) {
        const ImU32 col = i<3 ? green : i<6 ? red : cyan;
        c.dot(cx-24+i*6.0f,cy-36,1.8f,t.hasTelemetry && t.revPercent>(i+1)*100.0f/9 ? col : alpha(col,30));
    }
    auto button = [&](float x,float y,ImU32 col,const char* label) {
        c.dot(cx+x,cy+y,7,IM_COL32(2,3,4,255));
        c.dot(cx+x,cy+y,5.2f,col); c.dot(cx+x-1,cy+y-1,2,alpha(white,70));
        c.center(cx+x,cy+y+8,4.5f,IM_COL32(169,172,179,255),label,c.f.bold);
    };
    button(-40,-30,red,"PIT"); button(40,-30,green,"DRS");
    button(-46,-11,blue,"RAD"); button(46,-11,orange,"OT");
    button(-45,9,IM_COL32(240,217,42,255),"N"); button(45,9,white,"+10");
    button(-31,28,cyan,"BB"); button(31,28,red,"ERS");
    for (int side : {-1,1}) {
        const float x=cx+side*15.0f, y=cy+24;
        c.dot(x,y,9,IM_COL32(4,5,6,255));
        c.d->AddCircle(c.p(x,y),7*c.scale,side<0 ? orange : blue,20,2*c.scale);
        c.dot(x,y,5,IM_COL32(44,48,53,255));
        c.line(x,y,x+side*3,y-4,white,1.4f);
    }
    c.center(cx,cy+43,6,muted,"FORMULA",c.f.bold);
    const float angle = (t.hasTelemetry ? t.steer : 0)*240*pi/180;
    const float cs=std::cos(angle), sn=std::sin(angle);
    const auto pivot=c.p(cx,cy);
    for (int i=first;i<c.d->VtxBuffer.Size;++i) {
        auto& v=c.d->VtxBuffer[i].pos;
        const float x=v.x-pivot.x, y=v.y-pivot.y;
        v={pivot.x+x*cs-y*sn,pivot.y+x*sn+y*cs};
    }
}

void compactGraph(Canvas& c, const History& history, double now, float seconds) {
    constexpr float x=10,y=48,w=602,h=219;
    c.rect(x,y,w,h,IM_COL32(26,26,28,static_cast<int>(c.opacity*255)));
    c.line(x,y,x+w,y,IM_COL32(98,99,88,220));
    c.line(x,y+h,x+w,y+h,IM_COL32(88,90,93,230));
    c.d->PushClipRect(c.p(x,y-2),c.p(x+w,y+h+2),true);
    // Plain traces, like the reference: no grid, chart headings, or under-fill.
    for (int channel=0;channel<2;++channel) {
        std::vector<ImVec2> points;
        points.reserve(history.size());
        auto flush=[&]() {
            if (points.size()>1) {
                const auto color=channel==0 ? green : red;
                c.d->AddPolyline(points.data(),static_cast<int>(points.size()),alpha(color,25),0,5*c.scale);
                c.d->AddPolyline(points.data(),static_cast<int>(points.size()),color,0,2.2f*c.scale);
            }
            points.clear();
        };
        double previous=-1;
        for (std::size_t i=0;i<history.size();++i) {
            const auto& sample=history.at(i);
            if (sample.time<now-seconds-0.1 || sample.time>now) continue;
            if (previous>=0 && sample.time-previous>0.2) flush();
            previous=sample.time;
            points.push_back(c.p(x+w*(1-static_cast<float>((now-sample.time)/seconds)),
                y+h*(1-(channel==0 ? sample.throttle : sample.brake))));
        }
        flush();
    }
    c.d->PopClipRect();
}

void inputBars(Canvas& c, const Telemetry& t) {
    const std::array<float,3> inputs{t.clutch,t.brake,t.throttle};
    const std::array<ImU32,3> colors{blue,red,green};
    constexpr const char* labels[]{"C","B","T"};
    for (int i=0;i<3;++i) {
        const float x=636+i*39.0f;
        c.center(x+10,45,24,t.hasTelemetry ? white : muted,t.hasTelemetry ? number(inputs[i]*100,0) : "-",c.f.bold);
        c.rect(x,80,21,187,IM_COL32(37,37,39,230));
        c.rect(x+2,82,4,183,IM_COL32(49,49,51,170));
        if (t.hasTelemetry && inputs[i]>0) c.rect(x,267-187*inputs[i],21,187*inputs[i],colors[i]);
        c.center(x+10,71,8,muted,labels[i],c.f.bold);
    }
    c.text(782,72,46,white,gearText(t),c.f.bold);
    c.text(782,212,20,white,"kph",c.f.bold);
    c.text(781,234,33,white,t.hasTelemetry ? number(t.speed,0) : "---",c.f.bold);
}

void referenceHeader(Canvas& c, const Telemetry& t) {
    c.rect(0,0,899,37,IM_COL32(49,49,51,static_cast<int>(c.opacity*255)));
    c.text(12,6,23,white,t.hasLap ? "P"+std::to_string(t.position) : "P-",c.f.bold);
    c.fit(63,6,237,23,white,t.driver,c.f.bold);
    // Small line icons avoid relying on platform-specific icon fonts.
    c.outline(315,8,6,14,white,3,1.5f); c.dot(318,26,4,white);
    c.line(318,14,318,24,white,2);
    c.text(331,6,23,white,t.hasSession ? number(t.trackTemp,0)+"°C" : "--°C",c.f.bold);
    c.outline(425,9,13,20,white,0,1.5f); c.rect(428,12,7,6,white);
    c.line(438,12,443,17,white,1.5f); c.line(443,17,443,27,white,1.5f);
    c.text(452,6,23,white,t.hasStatus ? number(t.fuel,1)+"kg" : "--kg",c.f.bold);
    c.d->AddCircle(c.p(575,21),8*c.scale,white,20,1.5f*c.scale);
    c.line(572,8,578,8,white,2); c.line(575,14,575,21,white,1.5f); c.line(575,21,579,23,white,1.5f);
    c.text(592,6,23,white,t.hasLap ? lapTime(t.lapSeconds) : "--:--.---",c.f.bold);
    c.rect(766,0,133,37,t.hasLap && t.position>1 ? IM_COL32(191,17,16,245) : IM_COL32(65,65,67,230));
    c.text(774,5,8,white,"AHEAD",c.f.bold);
    c.right(890,12,23,white,t.hasLap && t.position>1 && t.gapAhead>0 ? number(t.gapAhead,2,true) : "--",c.f.bold);
}

std::string ersMode(const Telemetry& t) {
    return !t.hasStatus ? "--" : t.ersMode==0 ? "NONE" : t.ersMode==2 ? "HOTLAP" :
        t.ersMode==3 ? (t.regs2026 ? "BOOST" : "OVERTAKE") : "MEDIUM";
}

void details(Canvas& c, const Telemetry& t) {
    // One shallow continuation of the reference strip. Keep the same typeface,
    // flat background and small separators, with no duplicate driver header.
    c.rect(0,280,1120,120,IM_COL32(15,15,17,static_cast<int>(c.opacity*255)));
    c.line(10,286,1110,286,edge);
    for (float x : {372.0f,596.0f,832.0f}) c.line(x,298,x,390,alpha(edge,160));

    // The current lap clock is already in the header. Only add lap count,
    // personal best, sectors and last lap here.
    c.text(10,298,17,white,t.hasLap ? "LAP "+std::to_string(t.lap)+
        (t.hasSession ? " / "+std::to_string(t.totalLaps) : " / --") : "LAP -- / --",c.f.bold);
    c.right(358,300,14,white,"BEST  "+lapTime(t.bestLap),c.f.bold);
    for (int i=0;i<3;++i) {
        const float x=10+i*119.0f;
        const bool active=t.hasLap && t.sector==i, done=t.hasLap && t.sector>i;
        float value=done ? t.sectors[i] : 0;
        if (active) {
            value=t.lapSeconds;
            for (int j=0;j<i;++j) value-=t.sectors[j];
        }
        c.text(x,326,11,active ? green : muted,"S"+std::to_string(i+1)+(active ? "  LIVE" : ""),c.f.bold);
        c.text(x,341,20,white,value>0 ? number(value,3) : "--.---",c.f.bold);
    }
    c.text(10,378,12,muted,"LAST  "+lapTime(t.lastLap),c.f.bold);
    if (t.hasLap && t.invalidLap) c.right(358,378,12,red,"INVALID LAP",c.f.bold);
    else if (t.hasLap && t.penalties>0) c.right(358,378,12,orange,"PEN +"+std::to_string(t.penalties)+"s",c.f.bold);

    const bool ahead=t.hasLap && t.position>1 && t.gapAhead>0;
    const bool behind=t.hasLap && t.gapBehind>0;
    c.text(388,299,11,muted,"AHEAD",c.f.bold);
    c.fit(388,316,92,13,white,ahead && !t.aheadName.empty() ? t.aheadName : "---",c.f.bold);
    c.right(580,300,23,cyan,ahead ? number(t.gapAhead,3,true) : "---",c.f.bold);
    c.text(388,354,11,muted,"BEHIND",c.f.bold);
    c.fit(388,371,92,13,white,behind && !t.behindName.empty() ? t.behindName : "---",c.f.bold);
    c.right(580,355,23,orange,behind ? number(-t.gapBehind,3,true) : "---",c.f.bold);

    c.text(612,299,15,white,"ERS",c.f.bold);
    const auto energyColor=t.hasStatus ? t.ersPercent<20 ? orange : green : muted;
    c.right(816,295,24,energyColor,t.hasStatus ? number(t.ersPercent,0)+"%" : "--%",c.f.bold);
    for (int i=0;i<24;++i)
        c.rect(612+i*8.5f,324,6.5f,5,t.hasStatus && t.ersPercent>=(i+1)*100.0f/24 ? energyColor : alpha(energyColor,24));
    c.text(612,339,13,t.hasStatus && t.ersMode==3 ? orange : white,ersMode(t),c.f.bold);
    c.right(816,340,11,muted,t.hasStatus ? number(t.deployedMJ,2)+" MJ/LAP" : "-- MJ/LAP",c.f.bold);

    // Active aero and electrical overtake remain distinct. Missing packets
    // produce a neutral dash rather than an inferred activation state.
    const bool known=t.hasTelemetry && (t.regs2026 ? t.aeroKnown : t.hasStatus);
    const bool active=known && (t.regs2026 ? t.straightMode : t.drs==Drs::Open);
    const bool available=known && (t.regs2026 ? t.aeroAvailable : t.drs==Drs::Available);
    const std::string aero=!known ? "AERO --" : t.regs2026 ?
        (active ? "STRAIGHT MODE" : available ? "AERO READY" : "CORNER MODE") :
        (active ? "DRS OPEN" : available ? "DRS AVAILABLE" : "DRS CLOSED");
    c.text(612,377,12,active ? green : available ? cyan : muted,aero,c.f.bold);
    if (t.regs2026 && known && (t.overtake || t.overtakeAvailable))
        c.right(816,378,10,t.overtake ? orange : cyan,t.overtake ? "OT ACTIVE" : "OT READY",c.f.bold);

    c.text(848,299,14,white,"TIRES °C",c.f.bold);
    c.right(1110,301,11,muted,t.hasStatus ? compoundName(t.compound) : "---",c.f.bold);
    constexpr const char* names[]{"FL","FR","RL","RR"};
    for (int i=0;i<4;++i) {
        const float x=848+(i%2)*134.0f, y=332+(i/2)*37.0f;
        c.text(x,y+5,12,muted,names[i],c.f.bold);
        c.text(x+30,y,23,t.hasTelemetry ? tyreColor(t.tyreTemp[i]) : muted,
            t.hasTelemetry ? number(t.tyreTemp[i],0) : "--",c.f.bold);
    }
    // Secondary tire diagnostics are still available without consuming space.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(c.p(840,294),c.p(1120,400))) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Tire core / pressure / wear");
        ImGui::Separator();
        for (int i=0;i<4;++i) {
            const std::string row=std::string(names[i])+"   "+(t.hasTelemetry ? number(t.tyreTemp[i],0)+" °C   "+
                number(t.tyrePressure[i],1)+" PSI" : "-- °C   -- PSI")+"   "+
                (t.hasDamage ? number(t.tyreWear[i],0)+"% wear" : "-- wear");
            ImGui::TextUnformatted(row.c_str());
        }
        ImGui::EndTooltip();
    }
}


void configureStyle() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 4; s.FrameRounding = 2; s.FramePadding = {8,5}; s.ItemSpacing = {10,8};
    s.Colors[ImGuiCol_WindowBg] = {0.055f,0.075f,0.09f,0.99f};
    s.Colors[ImGuiCol_PopupBg] = s.Colors[ImGuiCol_WindowBg];
    s.Colors[ImGuiCol_CheckMark] = {0.64f,1,0.25f,1};
    s.Colors[ImGuiCol_SliderGrab] = {0.64f,1,0.25f,1};
    s.Colors[ImGuiCol_Button] = {0.14f,0.23f,0.19f,1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.22f,0.36f,0.22f,1};
}

bool saveFramebuffer(const std::string& path, int width, int height) {
    // A PPM capture of our own framebuffer permits deterministic visual QA
    // without capturing any other applications on the user's desktop.
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*3);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    std::ofstream out(path,std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y=height-1; y>=0; --y)
        out.write(reinterpret_cast<const char*>(pixels.data()+static_cast<std::size_t>(y)*width*3),width*3);
    return bool(out);
}

Options arguments(int argc, char** argv) {
    Options o;
    bool explicitHeight=false;
    for (int i=1; i<argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { if (++i >= argc) throw std::runtime_error("Missing value for " + a); return argv[i]; };
        if (a=="--live") o.live = true;
        else if (a=="--demo") o.live = false;
        else if (a=="--season") o.season = std::stoi(value());
        else if (a=="--port") o.port = std::stoi(value());
        else if (a=="--frames") o.frames = std::stoi(value());
        else if (a=="--width") o.width = std::stoi(value());
        else if (a=="--height") { o.height = std::stoi(value()); explicitHeight=true; }
        else if (a=="--demo-time") o.demoStart = std::stod(value());
        else if (a=="--screenshot") o.screenshot = value();
        else if (a=="--hidden") o.hidden = true;
        else if (a=="--paused") o.paused = true;
        else if (a=="--expanded") o.expanded = true;
        else if (a=="--compact") o.expanded = false;
        else if (a=="--setup") o.setup = true;
        else if (a=="--topmost") o.topmost = true;
        else if (a=="--decorated") o.borderless = false;
        else if (a=="--help") {
            std::cout << "APEX telemetry HUD\n"
                "  --demo | --live    Demo (default) or F1 25 / 2026 UDP\n"
                "  --season 2025|2026 Demo car regulations (default 2026)\n"
                "  --port 20777       UDP listening port\n"
                "  --topmost          Keep above other windows\n"
                "  --decorated        Show OS title bar and resize borders\n"
                "  --width N --height N --paused\n"
                "  --expanded        Show the slim F1 info strip (default)\n"
                "  --compact         Start with just the compact strip (F2)\n"
                "  --setup           Open connection/display settings\n"
                "  --frames N --hidden --screenshot output.ppm --demo-time seconds\n";
            std::exit(0);
        } else throw std::runtime_error("Unknown argument: " + a);
    }
    if (!explicitHeight) o.height=layoutHeight(o.width,o.expanded);
    if (o.port<1 || o.port>65535 || (o.season!=2025 && o.season!=2026) || o.frames<0 ||
        o.width<560 || o.width>7680 || o.height<140 || o.height>4320 || !std::isfinite(o.demoStart) || o.demoStart<0 || o.demoStart>3600)
        throw std::runtime_error("Invalid argument value. Use --help.");
    return o;
}
} // namespace

int main(int argc, char** argv) {
    Options o;
    try { o = arguments(argc,argv); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    glfwSetErrorCallback([](int code, const char* s) { std::cerr << "GLFW " << code << ": " << s << '\n'; });
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#endif
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER,GL_TRUE);
    glfwWindowHint(GLFW_DECORATED,o.borderless ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING,o.topmost ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE,o.hidden ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES,4);
    GLFWwindow* window = glfwCreateWindow(o.width,o.height,"APEX | F1 25 / 2026 Telemetry",nullptr,nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwSetWindowSizeLimits(window,560,140,GLFW_DONT_CARE,GLFW_DONT_CARE);
    glfwMakeContextCurrent(window); glfwSwapInterval(o.hidden ? 0 : 1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    configureStyle(); const Fonts fonts = loadFonts();
    if (!ImGui_ImplGlfw_InitForOpenGL(window,true) || !ImGui_ImplOpenGL3_Init("#version 330")) {
        std::cerr << "Failed to initialize renderer\n"; ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate(); return 1;
    }

    Simulator sim(o.season); UdpReceiver receiver; History history;
    if (o.live) receiver.open(o.port);
    // Warm the demo so the initial dashboard already shows a useful graph.
    const auto warm = [&]() {
        history.clear(); sim.reset(o.season); sim.data.driver = o.driver;
        for (int i=0; i<static_cast<int>(o.demoStart*120); ++i) {
            sim.step(1.0f/120); if (i%2==0) history.push(sim.clock,sim.data.throttle,sim.data.brake);
        }
    };
    if (!o.live) warm();
    auto changeSource = [&](bool live) {
        if (o.live==live) return;
        o.live = live; history.clear();
        if (live) receiver.open(o.port); else { receiver.close(); warm(); }
    };
    bool settings = o.setup, settingsSized = false, dragging = false;
    int restoreW=o.width,restoreH=o.height;
    auto toggleDetails = [&]() {
        if (settings) return;
        o.expanded=!o.expanded;
        int w,h; glfwGetWindowSize(window,&w,&h);
        glfwSetWindowSize(window,w,layoutHeight(w,o.expanded));
    };
    double last = glfwGetTime(), accumulator = 0, lastLiveSample = -1;
    float dragX = 0, dragY = 0;
    int rendered = 0, exitCode = 0, simTicks = 0;
    std::uint64_t liveEpoch = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        // Temporarily enlarge the same native window for setup; restore the
        // user's overlay dimensions on close, including any selected preset.
        if (settings && !settingsSized) {
            glfwGetWindowSize(window,&restoreW,&restoreH);
            glfwSetWindowSize(window,std::max(restoreW,800),std::max(restoreH,640));
            settingsSized=true;
        } else if (!settings && settingsSized) {
            glfwSetWindowSize(window,restoreW,restoreH); settingsSized=false;
        }
        const double now = glfwGetTime();
        const double dt = std::clamp(now-last,0.0,0.1); last = now;
        if (o.live) receiver.poll(now);
        else if (!o.paused) {
            accumulator += dt;
            while (accumulator >= 1.0/120) {
                sim.step(1.0f/120); accumulator -= 1.0/120;
                if (++simTicks%2==0) history.push(sim.clock,sim.data.throttle,sim.data.brake);
            }
        }
        if (o.live && receiver.decoder.epoch != liveEpoch) {
            history.clear(); liveEpoch = receiver.decoder.epoch; lastLiveSample = -1;
        }
        const bool connected = o.live && receiver.decoder.fresh(6,now,0.75);
        if (connected && now-lastLiveSample >= 1.0/60) {
            history.push(now,receiver.decoder.data.throttle,receiver.decoder.data.brake); lastLiveSample = now;
        }
        // Expire fields independently: a motion stream alone must not keep ERS,
        // pedals, or aero looking live after those packet types stop arriving.
        Telemetry t = o.live ? receiver.decoder.data : sim.data;
        if (o.live) {
            t.hasTelemetry &= receiver.decoder.fresh(6,now,0.75);
            t.hasLap &= receiver.decoder.fresh(2,now);
            t.hasStatus &= receiver.decoder.fresh(7,now,3);
            t.hasDamage &= receiver.decoder.fresh(10,now,3);
            t.hasMotion &= receiver.decoder.fresh(0,now);
            t.aeroKnown &= receiver.decoder.fresh(16,now,0.75);
            if (!t.aeroKnown) t.straightMode = t.aeroAvailable = t.overtake = t.overtakeAvailable = false;
        }
        int fbw=0, fbh=0; glfwGetFramebufferSize(window,&fbw,&fbh);
        if (fbw==0 || fbh==0) { glfwWaitEventsTimeout(0.05); continue; }
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { if (settings) settings=false; else glfwSetWindowShouldClose(window,1); }
            if (ImGui::IsKeyPressed(ImGuiKey_Space) && !o.live) o.paused = !o.paused;
            if (ImGui::IsKeyPressed(ImGuiKey_F1)) settings = !settings;
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) toggleDetails();
            if (ImGui::IsKeyPressed(ImGuiKey_F8)) { o.topmost = !o.topmost; glfwSetWindowAttrib(window,GLFW_FLOATING,o.topmost); }
            if (ImGui::IsKeyPressed(ImGuiKey_F9)) { o.borderless = !o.borderless; glfwSetWindowAttrib(window,GLFW_DECORATED,!o.borderless); }
        }
        const ImVec2 display = io.DisplaySize;
        const float baseHeight = o.expanded ? expandedHeight : compactHeight;
        const float scale = std::min(display.x/hudWidth,display.y/baseHeight);
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(display);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::Begin("##dashboard",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoBringToFrontOnFocus);
        Canvas c{ImGui::GetWindowDrawList(),fonts,scale,(display.x-hudWidth*scale)/2,(display.y-baseHeight*scale)/2,o.opacity};
        c.rect(0,37,899,243,IM_COL32(15,15,17,static_cast<int>(o.opacity*255)));
        referenceHeader(c,t);
        compactGraph(c,history,o.live ? now : sim.clock,o.graphSeconds);
        inputBars(c,t);
        steeringWheel(c,t);

        const std::string source = !o.live ? o.paused ? "PAUSED" : "DEMO "+std::to_string(o.season%100) : connected ?
            "LIVE "+std::to_string(t.format%100) : receiver.decoder.accepted ? "STALE" : "WAITING";
        const ImU32 sourceColor = o.live ? connected ? green : orange : white;
        if (c.button(914,3,101,30,"source",source,false,sourceColor)) ImGui::OpenPopup("Input source");
        if (c.button(1021,3,42,30,"setup","...")) settings = true;
        if (c.button(1069,3,40,30,"close","x",false,red)) glfwSetWindowShouldClose(window,1);
        if (c.button(1078,249,30,24,"details",o.expanded ? "-" : "+")) toggleDetails();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("F2: F1 telemetry details");
        // A small aero flag stays visible in the compact view when activated.
        // The info strip also shows the aero state and ERS readouts.
        const bool aeroActive = t.hasTelemetry && (t.regs2026 ? t.aeroKnown && t.straightMode : t.hasStatus && t.drs==Drs::Open);
        if (aeroActive) {
            c.rect(764,153,125,26,IM_COL32(18,77,10,220));
            c.center(826,158,14,green,t.regs2026 ? "STRAIGHT" : "DRS OPEN",fonts.bold);
        }
        if (o.expanded) details(c,t);
        const std::string notice = o.live ? !receiver.error.empty() ? receiver.error : !receiver.decoder.warning.empty() ?
            receiver.decoder.warning : !connected ? (receiver.decoder.accepted ? "SIGNAL STALE / CHECK GAME TELEMETRY" :
            "WAITING FOR F1 TELEMETRY / UDP "+std::to_string(receiver.localPort())) : "" : "";
        if (!notice.empty()) {
            c.rect(16,237,584,24,IM_COL32(14,14,16,230));
            c.fit(23,242,570,12,orange,notice,fonts.bold);
        }
        auto inputMenu = [&]() {
            if (ImGui::MenuItem("Demo / 2026",nullptr,!o.live && o.season==2026)) {
                o.season=2026; changeSource(false); warm();
            }
            if (ImGui::MenuItem("Demo / 2025",nullptr,!o.live && o.season==2025)) {
                o.season=2025; changeSource(false); warm();
            }
            if (ImGui::MenuItem("Live F1 UDP",nullptr,o.live)) changeSource(true);
            if (ImGui::MenuItem("Restart UDP",nullptr,false,o.live)) { receiver.open(o.port); history.clear(); }
        };
        if (ImGui::BeginPopup("Input source")) { inputMenu(); ImGui::EndPopup(); }
        if (ImGui::BeginPopupContextWindow("HUD controls",ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::BeginMenu("Input source")) { inputMenu(); ImGui::EndMenu(); }
            if (ImGui::MenuItem("F1 telemetry details","F2",o.expanded)) toggleDetails();
            ImGui::MenuItem("Pause demo","Space",&o.paused,!o.live);
            if (ImGui::MenuItem("Setup","F1")) settings=true;
            if (ImGui::MenuItem("Always on top","F8",&o.topmost)) glfwSetWindowAttrib(window,GLFW_FLOATING,o.topmost);
            if (ImGui::MenuItem("Quit","Esc")) glfwSetWindowShouldClose(window,1);
            ImGui::EndPopup();
        }
        // Only the header is a drag handle; input graphs and controls cannot
        // accidentally move the overlay. Popups also disable this behavior.
        const ImVec2 mouse=io.MousePos;
        const bool header = mouse.x>=c.p(0,0).x && mouse.x<c.p(900,0).x && mouse.y>=c.p(0,0).y && mouse.y<c.p(0,37).y;
        if (!settings && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId) && header && ImGui::IsMouseClicked(0)) {
            dragging=true; dragX=mouse.x; dragY=mouse.y;
        }
        if (!ImGui::IsMouseDown(0)) dragging=false;
        if (dragging) {
            int x,y; glfwGetWindowPos(window,&x,&y);
            glfwSetWindowPos(window,x+static_cast<int>(mouse.x-dragX),y+static_cast<int>(mouse.y-dragY));
        }
        ImGui::End(); ImGui::PopStyleVar();

        if (settings && settingsSized) ImGui::OpenPopup("Overlay setup");
        ImGui::SetNextWindowSize({600,580},ImGuiCond_Appearing);
        ImGui::SetNextWindowPos({display.x/2,display.y/2},ImGuiCond_Appearing,{0.5f,0.5f});
        if (ImGui::BeginPopupModal("Overlay setup",&settings,ImGuiWindowFlags_NoResize)) {
            ImGui::TextUnformatted("F1 25 / 2026 Season Pack");
            ImGui::TextWrapped("Enable UDP Telemetry in the game. Use this PC's IP, port 20777, and either the 2025 or 2026 UDP format. 60 Hz is recommended.");
            ImGui::Separator();
            ImGui::InputInt("UDP port",&o.port); o.port=std::clamp(o.port,1,65535);
            if (ImGui::Button("Connect / restart UDP",{275,32})) { o.live=true; receiver.open(o.port); history.clear(); }
            ImGui::SameLine(); if (ImGui::Button("Demo mode",{275,32})) changeSource(false);
            ImGui::InputText("Demo driver",o.driver,sizeof(o.driver)); sim.data.driver=o.driver;
            if (ImGui::Button(o.season==2026 ? "Demo season: 2026" : "Demo season: 2025")) {
                o.season=o.season==2026 ? 2025 : 2026; if (!o.live) warm();
            }
            ImGui::SameLine(); ImGui::Checkbox("Pause demo",&o.paused);
            ImGui::SliderFloat("Opacity",&o.opacity,0.40f,1.0f,"%.2f");
            ImGui::SliderFloat("Graph history",&o.graphSeconds,5,20,"%.0f seconds");
            if (ImGui::Checkbox("Always on top (F8)",&o.topmost)) glfwSetWindowAttrib(window,GLFW_FLOATING,o.topmost);
            ImGui::SameLine(); if (ImGui::Checkbox("Borderless (F9)",&o.borderless)) glfwSetWindowAttrib(window,GLFW_DECORATED,!o.borderless);
            if (ImGui::Checkbox("Show F1 info strip (F2)",&o.expanded)) restoreH=layoutHeight(restoreW,o.expanded);
            auto preset=[&](const char* name,int width) {
                if (ImGui::Button(name)) { restoreW=width; restoreH=layoutHeight(width,o.expanded); }
            };
            preset("Small / 560",560); ImGui::SameLine(); preset("Medium / 840",840); ImGui::SameLine(); preset("Large / 1120",1120);
            ImGui::TextWrapped("Drag the header to move. Right-click for controls. Use borderless/windowed game mode, and focus the overlay for keyboard shortcuts.");
            ImGui::Text("UDP %d / Received %llu / Rejected %llu",receiver.localPort(),
                static_cast<unsigned long long>(receiver.decoder.accepted),static_cast<unsigned long long>(receiver.decoder.rejected));
            if (ImGui::Button("Done",{562,34})) { settings=false; ImGui::CloseCurrentPopup(); }
            if (!settings) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Render(); glViewport(0,0,fbw,fbh);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++rendered;
        if (!o.screenshot.empty() && rendered==(o.frames>0 ? o.frames : 3)) {
            if (!saveFramebuffer(o.screenshot,fbw,fbh)) { std::cerr << "Could not write capture\n"; exitCode=1; }
        }
        glfwSwapBuffers(window);
        if (o.frames>0 && rendered>=o.frames) break;
        // Avoid burning a CPU core when swap interval is unavailable or bypassed.
        if (!o.hidden) {
            const double remaining = 1.0/144 - (glfwGetTime()-now);
            if (remaining>0) std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
        }
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return exitCode;
}

#ifdef _WIN32
// Use the Windows GUI subsystem so double-clicking the dashboard creates just
// the HUD. Redirected stdout/stderr remain available to build and capture tools.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
