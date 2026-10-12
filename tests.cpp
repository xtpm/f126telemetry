// Dependency-free tests: run through CTest, even with APEX_BUILD_HUD=OFF.
#include "udp.hpp"
#include <iostream>
#include <limits>
#include <random>
#include <thread>
#include <chrono>

using namespace apex;
namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void u16(std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t v) {
    b.at(p)=static_cast<std::uint8_t>(v); b.at(p+1)=static_cast<std::uint8_t>(v>>8);
}
void u32(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t v) {
    u16(b,p,static_cast<std::uint16_t>(v)); u16(b,p+2,static_cast<std::uint16_t>(v>>16));
}
void f32(std::vector<std::uint8_t>& b, std::size_t p, float f) { u32(b,p,std::bit_cast<std::uint32_t>(f)); }
std::vector<std::uint8_t> packet(int year, int id, int player=3, std::uint32_t frame=100) {
    std::vector<std::uint8_t> b(static_cast<std::size_t>(Decoder::packetSize(year,id)));
    u16(b,0,static_cast<std::uint16_t>(year)); b[2]=static_cast<std::uint8_t>(year%100);
    b[5]=1; b[6]=static_cast<std::uint8_t>(id); b[7]=42;
    u32(b,23,frame); b[27]=static_cast<std::uint8_t>(player); b[28]=255;
    return b;
}

void testPackets(int year) {
    Decoder d;
    auto b=packet(year,6);
    const std::size_t o=29+3*(year==2026 ? 59 : 60);
    u16(b,o,312); f32(b,o+2,0.95f); f32(b,o+6,-0.1f); f32(b,o+10,0.05f);
    b[o+14]=35; b[o+15]=8; u16(b,o+16,11800); b[o+18]=1; b[o+19]=82;
    for (int i=0; i<4; ++i) { b[o+34+i]=static_cast<std::uint8_t>(80+i*5); f32(b,o+(year==2026?39:40)+i*4,23.0f+i); }
    require(d.parse(b,1),"telemetry packet rejected");
    require(d.data.speed==312 && d.data.gear==8 && d.data.rpm==11800,"telemetry scalars incorrect");
    require(std::abs(d.data.clutch-0.35f)<0.001f,"clutch input incorrect");
    require(d.data.tyreTemp[0]==90 && d.data.tyreTemp[2]==80,"wheel order incorrect");
    require(d.data.tyrePressure[1]==26,"pressure offset incorrect");
    require(d.data.drs==Drs::Open,"DRS open not decoded");
    require(d.fresh(6,1.1) && !d.fresh(6,4),"freshness incorrect");
    const auto valid=b;
    // Every truncation length must be rejected without corrupting the last state.
    for (std::size_t n=0; n<valid.size(); ++n) {
        require(!d.parse({valid.data(),n},2),"truncation accepted");
        require(d.data.speed==312,"truncation mutated state");
    }
    b=valid; f32(b,o+2,std::numeric_limits<float>::quiet_NaN());
    require(!d.parse(b,2) && d.data.throttle==0.95f,"NaN accepted");
    b=valid; b[5]=99; require(!d.parse(b,2),"unsupported packet version accepted");
    b=valid; u16(b,0,2024); require(!d.parse(b,2),"unsupported format accepted");
    b=valid; b[27]=255; require(!d.parse(b,2),"invalid car accepted");
    b=valid; u32(b,23,99); u16(b,o,80);
    require(!d.parse(b,2) && d.data.speed==312,"old frame applied");

    b=packet(year,7);
    const std::size_t s=29+3*(year==2026 ? 59 : 55);
    b[s+3]=57; f32(b,s+5,30.2f); f32(b,s+13,1.2f); b[s+22]=1;
    b[s+26]=17; b[s+27]=6; f32(b,s+37,2'800'000); b[s+41]=3;
    f32(b,s+(year==2026?54:50),1'250'000);
    require(d.parse(b,2),"status packet rejected");
    require(d.data.ersPercent==70 && d.data.deployedMJ==1.25f && d.data.compound==17,"status incorrect");
    b=valid; u32(b,23,101); b[o+18]=0;
    require(d.parse(b,2.1) && d.data.drs==Drs::Available,"DRS availability lost");

    b=packet(year,4);
    const int stride=year==2026?60:57, nameOffset=year==2026?10:7;
    b[29]=6;
    std::memcpy(b.data()+30+3*stride+nameOffset,"TEST DRIVER",12);
    std::memcpy(b.data()+30+2*stride+nameOffset,"AHEAD",6);
    std::memcpy(b.data()+30+4*stride+nameOffset,"BEHIND",7);
    require(d.parse(b,2.2) && d.data.driver=="TEST DRIVER","participant offset incorrect");

    b=packet(year,2);
    const std::size_t l=29+3*57;
    u32(b,l+4,65432); u16(b,l+8,28400); u16(b,l+11,32000);
    u16(b,l+14,789); b[l+32]=4; b[l+33]=9; b[l+36]=2;
    b[29+2*57+32]=3; b[29+4*57+32]=5; u16(b,29+4*57+14,1234);
    require(d.parse(b,3),"lap packet rejected");
    require(d.data.position==4 && d.data.lap==9 && d.data.sector==2,"lap scalar offset incorrect");
    require(std::abs(d.data.gapAhead-0.789f)<0.001f && std::abs(d.data.gapBehind-1.234f)<0.001f,"interval incorrect");
    require(d.data.aheadName=="AHEAD" && d.data.behindName=="BEHIND","interval driver incorrect");

    b=packet(year,11); b[29]=3; b[30]=2; b[32]=1; b[33]=b[34]=b[35]=1;
    u32(b,36,89000); u16(b,40,28000); u16(b,43,32000); u16(b,46,29000); b[49]=15;
    require(d.parse(b,3.1) && d.data.bestLap==89 && d.data.bestSectors[2]==29,"history incorrect");
    b[29]=4; require(!d.parse(b,3.2),"other car history accepted");

    b=packet(year,10); f32(b,29+3*46+8,23);
    require(d.parse(b,3.2) && d.data.tyreWear[0]==23,"damage offset incorrect");
    b=packet(year,1); b[30]=34; b[31]=23; b[32]=27; b[36]=7; b[37]=year==2026 ? 13 : 0;
    u16(b,33,5891);
    require(d.parse(b,3.3) && d.data.trackId==7 && d.data.trackLength==5891,"session incorrect");
    if (year==2026) {
        b=packet(year,16); const std::size_t a=29+3*10;
        b[a]=b[a+1]=b[a+4]=b[a+5]=b[a+8]=1;
        require(d.parse(b,3.4) && d.data.straightMode && d.data.overtake && d.data.regs2026,"2026 aero incorrect");
        b[a+8]=0; u32(b,23,101);
        require(d.parse(b,3.5) && !d.data.regs2026,"pre-2026 car in 2026 format mislabeled");
    }
    // A session UID change must discard old names, lap times and status fields.
    b=valid; b[7]=43;
    require(d.parse(b,4) && !d.data.hasStatus && !d.data.hasLap && d.data.bestLap==0,"session reset incomplete");
    require(d.data.driver=="WAITING FOR DRIVER","driver survived session change");
    // Rejected first packet of a new session must not reset the current one.
    b[7]=44; f32(b,o+10,std::numeric_limits<float>::infinity());
    require(!d.parse(b,4.1) && d.data.speed==312,"malformed new session mutated state");
}

void testSimulation() {
    for (int year : {2025,2026}) {
        Simulator sim(year); bool braking=false, straight=false, drs=false, available=false, regen=false, deploy=false;
        int minGear=8,maxGear=1;
        for (int i=0; i<120*180; ++i) {
            float battery=sim.data.ersPercent; sim.step(1.0f/120);
            const auto& t=sim.data;
            require(std::isfinite(t.speed) && t.speed>=0 && t.speed<=360,"unrealistic demo speed");
            require(t.throttle>=0 && t.throttle<=1 && t.brake>=0 && t.brake<=1,"pedal out of range");
            require(t.ersPercent>=0 && t.ersPercent<=100,"battery out of range");
            require(t.gear>=1 && t.gear<=8,"gear out of range");
            require(!(t.brake>0.1f && (t.straightMode || t.drs==Drs::Open)),"aero open while braking");
            minGear=std::min(minGear,t.gear); maxGear=std::max(maxGear,t.gear);
            braking |= t.brake>0.9f; straight |= t.straightMode; drs |= t.drs==Drs::Open;
            available |= t.drs==Drs::Available || t.aeroAvailable;
            regen |= t.ersPercent>battery; deploy |= t.ersPercent<battery;
        }
        require(braking && available && regen && deploy && minGear<maxGear,"demo misses a driving phase");
        require(year==2026 ? straight && !drs : drs && !straight,"demo regulations mixed");
        require(sim.data.lap>=10 && sim.data.lastLap>80 && sim.data.lastLap<100,"lap rollover failed");
    }
    History h;
    for (int i=0; i<2000; ++i) h.push(i,0.5f,0);
    require(h.size()==History::capacity && h.at(0).time==560 && h.at(h.size()-1).time==1999,"ring buffer ordering incorrect");
    h.clear(); require(h.size()==0,"ring clear failed");
}

void testLoopback() {
    UdpReceiver receiver;
    require(receiver.open(0),"cannot open ephemeral UDP test port");
    require(receiver.localPort()>0,"ephemeral port was not assigned");
    const auto sender=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
#ifdef _WIN32
    require(sender!=INVALID_SOCKET,"cannot create UDP test sender");
#else
    require(sender>=0,"cannot create UDP test sender");
#endif
    sockaddr_in target{}; target.sin_family=AF_INET;
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    target.sin_port=htons(static_cast<std::uint16_t>(receiver.localPort()));
    auto b=packet(2026,6); u16(b,29+3*59,321);
    require(::sendto(sender,reinterpret_cast<const char*>(b.data()),static_cast<int>(b.size()),0,
        reinterpret_cast<sockaddr*>(&target),sizeof(target))==static_cast<int>(b.size()),"UDP test send failed");
    for (int i=0; i<100 && receiver.decoder.accepted==0; ++i) {
        receiver.poll(i*0.001);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#ifdef _WIN32
    closesocket(sender);
#else
    ::close(sender);
#endif
    require(receiver.decoder.data.speed==321 && receiver.decoder.accepted==1,"UDP loopback reception failed");
    receiver.close(); require(receiver.localPort()==0,"UDP port did not close");
}
}
int main() {
    try {
        testPackets(2025); testPackets(2026); testSimulation(); testLoopback();
        std::mt19937 rng(42); Decoder decoder;
        for (int i=0; i<2000; ++i) {
            std::vector<std::uint8_t> random(rng()%1600);
            for (auto& v:random) v=static_cast<std::uint8_t>(rng());
            decoder.parse(random,i);
        }
        std::cout << checks << " checks passed; both UDP formats, truncations, freshness, simulation and ring buffer.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
