#include "mkwv/runtime/engine_native.hpp"
#include "mkwv/runtime/runtime.hpp"
#include "memory.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#define MKWV_ENGINE_ENTRY(a) extern "C" void engine_ref_##a(CpuContext*);
#include "mkwv/runtime/engine_native_catalog.inc"
#undef MKWV_ENGINE_ENTRY
struct Entry { std::uint32_t address; void (*reference)(CpuContext*); };
const Entry entries[] = {
#define MKWV_ENGINE_ENTRY(a) {0x##a##u, engine_ref_##a},
#include "mkwv/runtime/engine_native_catalog.inc"
#undef MKWV_ENGINE_ENTRY
};

constexpr std::uint32_t A=0x80620000, B=0x80621000, C=0x80622000;
void write_float(std::uint32_t p,float v){std::uint32_t bits;std::memcpy(&bits,&v,4);Memory::Write32(p,bits);}
float read_float(std::uint32_t p){const auto bits=Memory::Read32(p);float v;std::memcpy(&v,&bits,4);return v;}
bool close(float a,float b){return std::fabs(a-b)<=2.0e-5f*(1+std::fabs(a));}

int main(){
    using namespace mkwv::runtime;
    Runtime runtime;
    if(!runtime.initialize("/tmp/mkwv-engine-overlay",MKWV_GUEST_BLOBS_DIR))return 1;
    std::mt19937 random(68);
    std::uniform_real_distribution<float> value(-2,2);
    for(const Entry& e:entries){
        for(unsigned iteration=0;iteration<80;++iteration){
            CpuContext cpu=runtime.cpu();cpu.gpr[1]=0x80610000;
            cpu.gpr[3]=A;cpu.gpr[4]=B;cpu.gpr[5]=C;cpu.gpr[6]=3;
            cpu.gqr[0]=0;cpu.gqr[2]=0x00040004;cpu.gqr[3]=0x00050005;cpu.gqr[6]=0x05070507;cpu.gqr[7]=0x08070807;
            for(unsigned i=14;i<32;++i)cpu.fpr[i].d=100.25+i;
            cpu.fpr[1].d=value(random);cpu.fpr[2].d=value(random);cpu.fpr[3].d=value(random);
            float data[3][64];
            for(auto& block:data)for(float& v:block)v=value(random);
            for(auto& block:data){block[0]+=4;block[5]+=4;block[10]+=4;}
            const bool compressed=e.address==0x800540F0||e.address==0x80054270;
            const bool animation=e.address==0x80052FD0||e.address==0x80053FB0||compressed;
            if(animation){
                // Eight F96 Hermite keys, with fractional values/tangents.
                for(unsigned i=0;i<8;++i){data[0][2+i*3]=float(i*2);data[0][3+i*3]=value(random);data[0][4+i*3]=value(random);}
                data[0][1]=1.0f/14.0f;
                cpu.fpr[1].d=iteration==0?-1:iteration==1?15:iteration%2?float(iteration%8)*2:float(iteration%14)+0.375f;
            }
            if(e.address==0x80085110||e.address==0x80085180||e.address==0x800851E0||e.address==0x80085940)cpu.fpr[1].d=(int(iteration)-40)*37.3125;
            if((e.address==0x80085110||e.address==0x80085180||e.address==0x800851E0||e.address==0x80085940)&&iteration<4){
                const double limits[]={65536.0,-65536.0,65535.5,-0.0};cpu.fpr[1].d=limits[iteration];
            }
            if(e.address==0x80085940){cpu.fpr[2].d=(int(iteration)-27)*18.375;cpu.fpr[3].d=(int(iteration)-52)*67.125;}
            // Supported in-place aliases: matrix concat and matrix/vector operations.
            if(iteration%3==1 && (e.address==0x80199D64||e.address==0x8019A91C||e.address==0x8019A970||e.address==0x8019ABE4||e.address==0x8019ACCC))cpu.gpr[5]=A;
            if(iteration%3==2 && (e.address==0x80199D64||e.address==0x8019A91C||e.address==0x8019A970||e.address==0x8019ABE4||e.address==0x8019ACCC))cpu.gpr[5]=B;
            auto reset=[&]{for(unsigned block=0;block<3;++block)for(unsigned i=0;i<64;++i)write_float(A+block*0x1000+i*4,data[block][i]);if(animation)Memory::Write16(A,8);
                if(compressed){
                    write_float(A+4,1.0f/14.0f);write_float(A+8,0.0078125f);write_float(A+12,-2.0f);
                    for(unsigned i=0;i<8;++i){
                        const unsigned v=unsigned((data[0][3+i*3]+2)*512);
                        const int slope=int(data[0][4+i*3]*32);
                        if(e.address==0x800540F0){Memory::Write16(A+16+i*6,i*64);Memory::Write16(A+18+i*6,v);Memory::Write16(A+20+i*6,slope*8);}
                        else Memory::Write32(A+16+i*4,(i*2<<24)|(v<<12)|(unsigned(slope)&4095));
                    }
                }};
            reset();CpuContext reference=cpu;e.reference(&reference);
            float expected[3][64];for(unsigned block=0;block<3;++block)for(unsigned i=0;i<64;++i)expected[block][i]=read_float(A+block*0x1000+i*4);
            reset();
            if(!try_native_engine(e.address,&cpu)){std::fprintf(stderr,"native declined %08X r2=%08X period=%g gqr0=%08X\n",e.address,cpu.gpr[2],read_float(cpu.gpr[2]-28960),cpu.gqr[0]);return 1;}
            for(unsigned block=0;block<3;++block)for(unsigned i=0;i<64;++i){
                const float got=read_float(A+block*0x1000+i*4);
                // The animation header has integer halfwords rather than floating data.
                if(animation&&block==0&&(i==0||(compressed&&i>=4&&i<16)))continue;
                if(!close(expected[block][i],got)){
                    std::fprintf(stderr,"%08X iteration=%u block=%u word=%u ppc=%g native=%g\n",e.address,iteration,block,i,expected[block][i],got);return 1;
                }
            }
            if(animation||e.address==0x80085110||e.address==0x80085180||e.address==0x8019AC68||e.address==0x8019ACAC||e.address==0x8019ADE0||e.address==0x80041B70){
                const bool paired=e.address==0x8019ACAC||e.address==0x8019ADE0||e.address==0x80041B70;
                const float original=paired?reference.fpr[1].paired.ps0:float(reference.fpr[1].d);
                const float native=paired?cpu.fpr[1].paired.ps0:float(cpu.fpr[1].d);
                if(!close(original,native)||(paired&&!close(reference.fpr[1].paired.ps1,cpu.fpr[1].paired.ps1))){std::fprintf(stderr,"%08X scalar ppc=%g native=%g\n",e.address,original,native);return 1;}
            }
            if ((e.address==0x80199FC8||e.address==0x8019A0C0) && cpu.gpr[3]!=reference.gpr[3])return 1;
            // Native routines preserve PPC nonvolatile state without host-context round trips.
            for(unsigned i=14;i<32;++i)if(cpu.fpr[i].d!=100.25+i)return 1;
        }
    }
    // A rejected native call must leave both context and RAM untouched so the
    // wrapper can execute the original routine from exactly the original state.
    auto declined=[&](std::uint32_t address, CpuContext cpu){
        const CpuContext before=cpu;
        std::uint32_t ram[3][64];
        for(unsigned block=0;block<3;++block)for(unsigned i=0;i<64;++i)ram[block][i]=Memory::Read32(A+block*0x1000+i*4);
        if(try_native_engine(address,&cpu)||std::memcmp(&before,&cpu,sizeof(cpu))){std::fprintf(stderr,"fallback changed context or accepted %08X r3=%08X\n",address,before.gpr[3]);return false;}
        for(unsigned block=0;block<3;++block)for(unsigned i=0;i<64;++i)if(ram[block][i]!=Memory::Read32(A+block*0x1000+i*4))return false;
        return true;
    };
    CpuContext cpu=runtime.cpu();cpu.gpr[3]=A;cpu.gpr[4]=B;cpu.gpr[5]=C;cpu.gqr[0]=1;
    if(!declined(0x80199D30,cpu))return 1;
    cpu.gqr[0]=0;
    for(unsigned i=0;i<12;++i)write_float(A+i*4,0);
    if(!declined(0x80199FC8,cpu)||!declined(0x8019A0C0,cpu)||!declined(0x8019AC24,cpu))return 1;
    cpu.gpr[3]=0xFFFFFFFFu; if(!declined(0x80199D30,cpu))return 1;
    cpu.gpr[3]=A;cpu.fpr[1].d=70000;
    if(!declined(0x80085110,cpu))return 1;
    cpu.fpr[1].d=2;cpu.gqr[3]=0x00050005;
    Memory::Write16(A,0);if(!declined(0x80053FB0,cpu))return 1;
    Memory::Write16(A,8);cpu.gqr[6]=0;
    if(!declined(0x800540F0,cpu)||!declined(0x80054270,cpu))return 1;
    cpu.gpr[3]=A;cpu.gpr[4]=B;cpu.gpr[5]=B+4;cpu.gpr[6]=2;
    if(!declined(0x80199E30,cpu))return 1;
    set_native_engine(false);if(!declined(0x80199D04,cpu))return 1;set_native_engine(true);
    std::printf("engine_native: %zu entries x 80 PPC differential cases passed\n",sizeof(entries)/sizeof(entries[0]));
    return 0;
}
