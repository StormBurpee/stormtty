// Matching GlassMaterial reference. Shader Model 5.0; source and target MUST be distinct.
// Material pass: blending OFF, all RGBA writes, matching zero-origin viewport.
// Linear premultiplied RGBA. Foreground labels are a subsequent pass, never this source.
Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0); // MIN_MAG_MIP_LINEAR; CLAMP U/V/W.
cbuffer GlassParameters : register(b0) {
    float2 Center; float2 Size;
    float Radius; float Rim; float Thickness; float IndexOfRefraction;
    float MaxDisplacement; float BlurRadius; float HighlightStrength; float Enabled;
    float2 SourceSize; float2 Reserved;
};
float sign_nonzero(float n) { return n<0 ? -1 : 1; }
float4 sample_pixel(float2 p) {
    return Source.SampleLevel(LinearClamp,clamp(p,.5,SourceSize-.5)/SourceSize,0);
}
float4 glass_ps(float4 position : SV_Position) : SV_Target {
    const float2 pixel=position.xy;
    // Pixel-matched output viewport is required. Off bypasses all filtering/math.
    if(Enabled==0) return Source.Load(int3(int2(pixel),0));
    float2 local=pixel-Center;
    float2 q=abs(local)-(Size*.5-Radius);
    float2 outer=max(q,0);
    float outer_length=length(outer);
    float distance=outer_length+min(max(q.x,q.y),0)-Radius;
    if(distance>=0) return Source.Load(int3(int2(pixel),0));
    float coverage=saturate(-distance);
    coverage=coverage*coverage*(3-2*coverage);
    float2 outward;
    if(outer_length>1e-6) outward=outer/outer_length*float2(sign_nonzero(local.x),sign_nonzero(local.y));
    else if(q.x>q.y) outward=float2(sign_nonzero(local.x),0);
    else outward=float2(0,sign_nonzero(local.y));
    const float pi=3.14159265358979323846;
    float t=saturate(-distance/Rim);
    float height=Thickness*sin(t*pi*.5);
    float slope=t>=1 ? 0 : Thickness*pi/(2*Rim)*cos(t*pi*.5);
    float3 normal=float3(outward*slope,1)/sqrt(1+slope*slope);
    float eta=1/IndexOfRefraction;
    float coefficient=eta*normal.z-sqrt(max(0,1-eta*eta*(1-normal.z*normal.z)));
    float rz=-eta+coefficient*normal.z;
    float2 shift=normal.xy*coefficient*height/max(-rz,.01);
    float magnitude=length(shift);
    if(magnitude>MaxDisplacement && magnitude>0) shift*=MaxDisplacement/magnitude;
    float2 source_pixel=pixel+shift;
    float4 color;
    if(BlurRadius==0) color=sample_pixel(source_pixel);
    else {
        const float weights[3]={.25,.5,.25};
        color=0;
        [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
            color+=sample_pixel(source_pixel+float2(x,y)*BlurRadius)*weights[x+1]*weights[y+1];
    }
    float facing=max(0,dot(float2(-.6,-.8),normal.xy));
    float highlight=HighlightStrength*(1-normal.z)*facing;
    color.rgb=lerp(color.rgb,color.aaa,highlight);
    return lerp(Source.Load(int3(int2(pixel),0)),color,coverage);
}
float4 fullscreen_vs(uint id : SV_VertexID) : SV_Position {
    float2 uv=float2((id<<1)&2,id&2);
    return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
