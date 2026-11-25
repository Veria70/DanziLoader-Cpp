

R"hlsl(
#define RS "RootConstants(num32BitConstants=7, b0), SRV(t0), UAV(u0), UAV(u1), UAV(u2)"
cbuffer Span : register(b0) {
    uint first;        
    uint skew;         
    uint size;         
    uint pieces;
    uint out_at;       
    uint compare;      
    uint inline_limit; 
};
ByteAddressBuffer now : register(t0);


RWByteAddressBuffer hashes : register(u0);
RWByteAddressBuffer differs : register(u1);
RWByteAddressBuffer inline_buf : register(u2);
groupshared uint4 s_hash[64];
groupshared uint s_slot;
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint idx = gid.x;
    uint lane = gtid.x;
    if (idx >= pieces) return;
    uint piece = first + idx;
    uint lo = piece * 65536u;
    lo = lo > skew ? lo - skew : 0u;
    uint hi = min((piece + 1u) * 65536u - skew, size);
    lo = lo & ~15u;
    hi = (hi + 15u) & ~15u;
    uint span_bytes = hi > lo ? (hi - lo) : 0u;
    uint4 h = uint4(0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu) ^ (lane * 0x9E3779B9u);
    [loop]
    for (uint off = lane * 16u; off < span_bytes; off += 1024u) {
        h = (h ^ now.Load4(lo + off)) * uint4(0x01000193u, 0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du);
        h = h.yzwx ^ (h >> 15);
    }
    s_hash[lane] = h;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            uint4 a = s_hash[lane];
            uint4 b = s_hash[lane + stride];
            a = (a ^ (b * uint4(0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu, 0x9E3779B9u)))
                * uint4(0x01000193u, 0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du);
            s_hash[lane] = a.yzwx ^ (a >> 15);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u) {
        uint4 final_h = s_hash[0];
        s_slot = 0xFFFFFFFFu;
        if (compare != 0u) {
            bool changed = any(final_h != hashes.Load4(piece * 16u));
            if (changed) {
                
                
                if (compare == 2u || compare == 4u) {
                    hashes.Store4(piece * 16u, final_h);
                }
                if (compare == 3u || compare == 4u) {
                    uint cur = inline_buf.Load(0u);
                    if (cur < 0xFFFFFFFEu && (int)(cur - inline_limit) < 0) {
                        uint ticket = 0u;
                        inline_buf.InterlockedAdd(0u, 1u, ticket);
                        if (ticket < 0xFFFFFFFEu && (int)(ticket - inline_limit) < 0) {
                            s_slot = ticket & 2047u;
                            differs.Store(out_at + idx * 4u, ticket + 2u);
                        } else {
                            differs.Store(out_at + idx * 4u, 1u);
                        }
                    } else {
                        differs.Store(out_at + idx * 4u, 1u);
                    }
                } else {
                    differs.Store(out_at + idx * 4u, 1u);
                }
            } else {
                differs.Store(out_at + idx * 4u, 0u);
            }
        } else {
            hashes.Store4(piece * 16u, final_h);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint slot = s_slot;
    if (slot != 0xFFFFFFFFu) {
        uint dst_base = 65536u + slot * 65600u;
        [loop]
        for (uint off = lane * 16u; off < span_bytes; off += 1024u) {
            inline_buf.Store4(dst_base + off, now.Load4(lo + off));
        }
    }
}
)hlsl"
