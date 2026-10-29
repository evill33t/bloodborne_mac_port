// SPDX-License-Identifier: GPL-2.0-or-later
// Foreign geometry/texture conversion onto a Bloodborne rig. The destination's
// animation, behavior and hitbox assets are a separate part of the import pipeline.
using SoulsFormats;
using System.Numerics;
using System.Text.Json;
using System.Text.RegularExpressions;

internal record BoneMatch(string Source,string Target,bool Direct);
internal record SkinBake(FLVER2 Model,TPF Textures,List<BoneMatch> Bones,int Vertices,int Triangles,double DirectWeightShare,List<string> TextureFallbacks) {public float ProportionScale {get;init;}=1;}
internal static class SkinRetarget {
    const int VertexLimit=2000000;
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static string TextureName(string name)=>Leaf(name).Split('.')[0];
    static string BoneName(string name){
        var value=new string(name.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
        foreach(var prefix in new[]{"bip001","bip01"})if(value.StartsWith(prefix,StringComparison.Ordinal))return value[prefix.Length..];return value;
    }
    internal static Matrix4x4[] World(IReadOnlyList<FLVER.Node> nodes){
        if(nodes.Count==0 || nodes.Count>4096)throw new InvalidDataException("Skeleton bone count is outside the import limit");
        var world=new Matrix4x4[nodes.Count];var status=new byte[nodes.Count];
        Matrix4x4 At(int i){
            if(i<0 || i>=nodes.Count)throw new InvalidDataException("Invalid skeleton parent");if(status[i]==2)return world[i];if(status[i]==1)throw new InvalidDataException("Cyclic skeleton");status[i]=1;
            var p=nodes[i].ParentIndex;world[i]=nodes[i].ComputeLocalTransform()*(p==-1?Matrix4x4.Identity:At(p));status[i]=2;return world[i];
        }
        for(int i=0;i<nodes.Count;++i)At(i);return world;
    }
    static FLVER2.Texture Copy(FLVER2.Texture t)=>new(t.ParamName,t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C);
    internal static byte[] RgbaTexture(byte[] pixel){
        if(pixel.Length!=4)throw new ArgumentException("Expected one RGBA pixel");var dds=new DDS{dwWidth=1,dwHeight=1,dwMipMapCount=1,dwPitchOrLinearSize=4};
        dds.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;dds.ddspf.dwRGBBitCount=32;dds.ddspf.dwRBitMask=0xff;dds.ddspf.dwGBitMask=0xff00;dds.ddspf.dwBBitMask=0xff0000;dds.ddspf.dwABitMask=0xff000000;return dds.Write(pixel);
    }
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z)&&p.LengthSquared()<1000000;
    // Native behavior packfiles refer to their character, graph, script and clips
    // by embedded ASCII names. Equal-length string edits preserve every fixup.
    internal static byte[] RenameBehavior(Memory<byte> input,string from,string to){
        if(from.Length!=5 || to.Length!=5 || !Regex.IsMatch(from,@"\Ac\d{4}\z") || !Regex.IsMatch(to,@"\Ac\d{4}\z"))throw new InvalidDataException("Behavior renaming requires four-digit character IDs");
        var result=input.ToArray();if(result.Length<64 || BitConverter.ToUInt32(result)!=0x57e0e057)throw new InvalidDataException("Expected a native Havok behavior packfile");
        for(int i=64;i<result.Length;){
            if(result[i]<32 || result[i]>126){++i;continue;}var start=i;while(i<result.Length && result[i]>=32 && result[i]<=126)++i;
            if(i==result.Length || result[i]!=0 || i-start<5)continue;
            var text=System.Text.Encoding.ASCII.GetString(result,start,i-start);
            var renamed=Regex.Replace(text,@"(?<![A-Za-z0-9])"+from+@"(?![0-9])",to,RegexOptions.IgnoreCase|RegexOptions.CultureInvariant);
            if(text!=renamed)System.Text.Encoding.ASCII.GetBytes(renamed).CopyTo(result,start);
        }return result;
    }
    static string TextureSlot(string name)=>name.EndsWith("Texture",StringComparison.Ordinal)?name[..^7]:name;
    // These native variants use the same Bloody character shader, texture slots
    // and vertex inputs as C[ARSN]. Their MTD selects alpha-test/blend behavior;
    // both also explicitly disable adaptive tessellation.
    // Source material labels are unreliable: an "_Alp" label may use an Edge MTD.
    static string CharacterMtd(string sourceMtd,string nativeMtd) {
        string suffix=Leaf(sourceMtd).ToLowerInvariant() switch {
            "ps_hair[ds]_edge.mtd"=>"_e",
            "ps_hair[ds]_alp.mtd"=>"_al",
            _=>""
        };
        return suffix.Length==0?nativeMtd:nativeMtd[..^4]+suffix+".mtd";
    }
    // TPF's format byte is game-specific. Preserve the DDS pixel representation
    // and select the Bloodborne/PS4 format instead of copying that source byte.
    internal static byte Ps4Format(DDS dds)=>(int)dds.GetDXGIFormat() switch {
        28 or 29=>105,87 or 91=>9,71=>0,72=>108,74 or 75=>3,77=>5,78=>110,80=>103,83=>104,95=>100,98=>102,99=>112,
        var format=>throw new InvalidDataException("Unsupported DDS pixel format for PS4 conversion: "+format)
    };
    // SoulsFormats' DDS splitter quarters byte counts instead of clamping each
    // mip dimension. That truncates the tail of rectangular weapon textures.
    internal static List<Headerizer.Image> DdsMipImages(byte[] bytes) {
        var h=new DDS(bytes);
        if(h.dwWidth<1||h.dwHeight<1||h.dwWidth>8192||h.dwHeight>8192||h.dwDepth>1||h.dwCaps2!=0||
            h.header10 is {} dx&&(dx.arraySize!=1||dx.resourceDimension!=DDS.DIMENSION.TEXTURE2D))throw new InvalidDataException("Expected a bounded two-dimensional texture");
        var (block,size)=(int)h.GetDXGIFormat() switch {
            28 or 29 or 87 or 91=>(1,4),71 or 72 or 80=>(4,8),74 or 75 or 77 or 78 or 83 or 95 or 98 or 99=>(4,16),
            _=>throw new InvalidDataException("Unsupported mip pixel format")
        };
        int levels=Math.Max(1,h.dwMipMapCount),width=h.dwWidth,height=h.dwHeight,offset=h.DataOffset;
        if(levels>1+BitOperations.Log2((uint)Math.Max(width,height)))throw new InvalidDataException("Invalid texture mip count");
        var image=new Headerizer.Image();
        for(int i=0;i<levels;i++) {
            int count=checked(((width+block-1)/block)*((height+block-1)/block)*size);
            if(count>bytes.Length-offset)throw new InvalidDataException("Truncated texture mip");
            image.subImages.Add(bytes.AsSpan(offset,count).ToArray());offset+=count;width=Math.Max(1,width/2);height=Math.Max(1,height/2);
        }
        return [image];
    }
    internal static TPF.Texture NativeTexture(string name,byte[] bytes) {
        var h=new DDS(bytes);var images=DdsMipImages(bytes);
        return new TPF.Texture(TPF.TPFPlatform.PS4){Name=name,Format=Ps4Format(h),Flags1=0,Type=TPF.TexType.Texture,
            Mipmaps=checked((byte)images[0].subImages.Count),Header=new(){DXGIFormat=(int)h.GetDXGIFormat(),Width=checked((short)h.dwWidth),Height=checked((short)h.dwHeight),TextureCount=1,Unk2=0xD},
            Bytes=Headerizer.WritePS4Images(images,h,TPF.TexType.Texture)};
    }
    // Legacy DDS FourCCs do not encode sRGB. Native Bloodborne albedo/sky
    // textures do; tagging their color view preserves every block, mip and
    // alpha bit while preventing nonlinear source RGB being lit as linear RGB.
    internal static byte[] NativeSrgbColor(byte[] bytes) {
        var header=new DDS(bytes);int format=(int)header.GetDXGIFormat();
        int srgb=format switch {28=>29,87=>91,71=>72,74=>75,77=>78,98=>99,29 or 91 or 72 or 75 or 78 or 99=>format,
            _=>throw new InvalidDataException("Unsupported source color texture format: "+format)};
        if(format==srgb)return bytes;
        int offset=header.DataOffset;
        header.header10??=new DDS.HEADER_DXT10();header.header10.dxgiFormat=(DDS.DXGI_FORMAT)srgb;
        header.ddspf.dwFlags=DDS.DDPF.FOURCC;header.ddspf.dwFourCC="DX10";
        return header.Write(bytes.AsSpan(offset).ToArray());
    }
    // BC5 supplies tangent-normal RG only. Bloodborne reconstructs direction
    // from RG; its normal-map B also modulates gloss. Preserve RG and supply a
    // neutral B=1 instead of allowing BC5's implicit B=0 to erase shininess.
    internal static byte[] NativeBc5Normal(byte[] bytes) {
        var header=new DDS(bytes);
        if(header.GetDXGIFormat()!=DDS.DXGI_FORMAT.BC5_UNORM||header.dwWidth<1||header.dwHeight<1||
            header.dwWidth>8192||header.dwHeight>8192||header.dwDepth>1||header.dwCaps2!=0||
            header.header10 is {} h&&(h.arraySize!=1||h.resourceDimension!=DDS.DIMENSION.TEXTURE2D))
            throw new InvalidDataException("Expected a two-dimensional BC5_UNORM normal map");
        int levels=Math.Max(1,header.dwMipMapCount);
        if(levels>1+System.Numerics.BitOperations.Log2((uint)Math.Max(header.dwWidth,header.dwHeight)))throw new InvalidDataException("Invalid BC5 mip count");
        static byte Channel(ReadOnlySpan<byte> block,int texel) {
            Span<int> values=stackalloc int[8];values[0]=block[0];values[1]=block[1];
            if(values[0]>values[1])for(int i=1;i<=6;i++)values[i+1]=((7-i)*values[0]+i*values[1])/7;
            else {for(int i=1;i<=4;i++)values[i+1]=((5-i)*values[0]+i*values[1])/5;values[6]=0;values[7]=255;}
            ulong indices=0;for(int i=0;i<6;i++)indices|=(ulong)block[i+2]<<(8*i);return (byte)values[(int)(indices>>(3*texel)&7)];
        }
        using var pixels=new MemoryStream();int width=header.dwWidth,height=header.dwHeight,offset=header.DataOffset;
        for(int level=0;level<levels;level++) {
            int blocksX=(width+3)/4,blocksY=(height+3)/4,size=checked(blocksX*blocksY*16);
            if(bytes.Length-offset<size||(long)pixels.Length+(long)width*height*4>128*1024*1024)throw new InvalidDataException("BC5 normal is truncated or exceeds the conversion budget");
            var mip=bytes.AsSpan(offset,size);offset+=size;
            var rgba=new byte[checked(width*height*4)];
            for(int by=0;by<blocksY;by++)for(int bx=0;bx<blocksX;bx++) {
                var block=mip.Slice((by*blocksX+bx)*16,16);
                for(int y=0;y<4;y++)for(int x=0;x<4;x++) {
                    int px=bx*4+x,py=by*4+y;if(px>=width||py>=height)continue;int dest=(py*width+px)*4,texel=y*4+x;
                    rgba[dest]=Channel(block[..8],texel);rgba[dest+1]=Channel(block[8..],texel);rgba[dest+2]=255;rgba[dest+3]=255;
                }
            }
            pixels.Write(rgba);width=Math.Max(1,width/2);height=Math.Max(1,height/2);
        }
        var output=new DDS{dwWidth=header.dwWidth,dwHeight=header.dwHeight,dwMipMapCount=levels,dwPitchOrLinearSize=header.dwWidth*4};
        output.dwFlags|=DDS.DDSD.PITCH;if(levels>1){output.dwFlags|=DDS.DDSD.MIPMAPCOUNT;output.dwCaps|=DDS.DDSCAPS.COMPLEX|DDS.DDSCAPS.MIPMAP;}
        output.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;output.ddspf.dwRGBBitCount=32;
        output.ddspf.dwRBitMask=0xff;output.ddspf.dwGBitMask=0xff00;output.ddspf.dwBBitMask=0xff0000;output.ddspf.dwABitMask=0xff000000;return output.Write(pixels.ToArray());
    }
    internal static float HumanoidScale(IReadOnlyList<FLVER.Node> source,IReadOnlyList<FLVER.Node> native) {
        float Height(IReadOnlyList<FLVER.Node> nodes) {
            var world=World(nodes);int Find(string name)=>Enumerable.Range(0,nodes.Count).SingleOrDefault(i=>BoneName(nodes[i].Name)==name,-1);
            int head=Find("head"),left=Find("lfoot"),right=Find("rfoot");
            if(head<0||left<0||right<0)throw new InvalidDataException("Humanoid proportions require unique head and foot bones");
            float height=world[head].Translation.Y-(world[left].Translation.Y+world[right].Translation.Y)*.5f;
            if(!float.IsFinite(height)||height is <.5f or >4)throw new InvalidDataException("Unsupported humanoid skeletal height");return height;
        }
        float scale=Height(source)/Height(native);
        if(scale is <.5f or >1.5f)throw new InvalidDataException("Incompatible humanoid proportions");return scale;
    }
    // With an empty palette, FromSoftware's weighted meshes use global node indices.
    internal static int Bone(IFlverMesh mesh,int index){
        if(mesh is FLVER2.Mesh f){if(f.BoneIndices.Count==0)return index;if(index<0 || index>=f.BoneIndices.Count)throw new InvalidDataException("Invalid mesh bone palette index");return f.BoneIndices[index];}
        var palette=((FLVER0.Mesh)mesh).BoneIndices;if(palette.Length==0)return index;if(index<0 || index>=palette.Length)throw new InvalidDataException("Invalid mesh bone palette index");return palette[index];
    }
    internal static SkinBake Bake(Memory<byte> sourceBytes,Memory<byte> nativeBytes,IReadOnlyList<TPF.Texture> textures,IReadOnlySet<int>? masks=null,IReadOnlyDictionary<string,string>? overrides=null,string textureOwner="c9998",string? sourceProfile=null){
        if(!Regex.IsMatch(textureOwner,@"\Ac\d{4,5}\z"))throw new ArgumentException("Invalid texture owner");
        string OwnedTexture(string name)=>textureOwner+"_import_"+name;
        IFlver source=FLVER2.Is(sourceBytes.ToArray())?FLVER2.Read(sourceBytes):FLVER0.Is(sourceBytes.ToArray())?FLVER0.Read(sourceBytes):throw new InvalidDataException("Unsupported source FLVER");
        var native=FLVER2.Read(nativeBytes);if(native.Header.Version is not(0x20013 or 0x20014) || native.Header.BigEndian)throw new InvalidDataException("Choose a Bloodborne template FLVER");
        if(source.Meshes.Count>10000 || source.Meshes.Sum(m=>(long)m.Vertices.Count)>VertexLimit)throw new InvalidDataException("Source character exceeds the mesh budget");
        var sw=World(source.Nodes);var tw=World(native.Nodes);var targets=native.Nodes.Select((n,i)=>(key:BoneName(n.Name),i)).GroupBy(v=>v.key).ToDictionary(g=>g.Key,g=>g.First().i);
        var names=native.Nodes.Select((n,i)=>(n.Name,i)).GroupBy(v=>v.Name).ToDictionary(g=>g.Key,g=>g.First().i);var mapping=Enumerable.Repeat(-1,source.Nodes.Count).ToArray();var direct=new bool[mapping.Length];var bindSource=Enumerable.Repeat(-1,mapping.Length).ToArray();
        var aliases=new Dictionary<string,string>{{"lforearmtwist","lforetwist"},{"rforearmtwist","rforetwist"},{"lforearmtwist1","lforetwist"},{"rforearmtwist1","rforetwist"},{"luparmtwist1","luparmtwist"},{"ruparmtwist1","ruparmtwist"}};
        for(int i=0;i<mapping.Length;++i){
            var name=source.Nodes[i].Name;var key=BoneName(name);
            if(overrides?.TryGetValue(name,out var target)==true){if(!names.TryGetValue(target,out mapping[i]))throw new InvalidDataException("Mapped target bone does not exist: "+target);}
            else {if(aliases.TryGetValue(key,out var alias))key=alias;if(targets.TryGetValue(key,out var n))mapping[i]=n;}
            direct[i]=mapping[i]>=0;if(direct[i])bindSource[i]=i;
        }
        int Match(int i){
            if(i<0 || i>=mapping.Length)throw new InvalidDataException("Vertex references an invalid source bone");if(mapping[i]>=0)return mapping[i];
            var p=source.Nodes[i].ParentIndex;if(p<0)throw new InvalidDataException("No compatible destination bone for "+source.Nodes[i].Name+". Supply an explicit bone mapping.");mapping[i]=Match(p);bindSource[i]=bindSource[p];return mapping[i];
        }
        // Resolve every referenced bone lazily: unreferenced source helper nodes need no mapping.
        var changed=new Matrix4x4[mapping.Length];var normals=new Matrix4x4[mapping.Length];var ready=new bool[mapping.Length];
        int Target(int i){
            var target=Match(i);if(target>255)throw new InvalidDataException("Destination bone index exceeds the template's byte layout");
            if(!ready[i]){if(!Matrix4x4.Invert(sw[bindSource[i]],out var inverse))throw new InvalidDataException("Singular source bind pose");changed[i]=inverse*tw[target];if(!Matrix4x4.Invert(changed[i],out var inv))throw new InvalidDataException("Singular retarget transform");normals[i]=Matrix4x4.Transpose(inv);ready[i]=true;}return target;
        }
        var template=native.Meshes.FirstOrDefault(m=>m.UseBoneWeights && native.Materials[m.MaterialIndex].MTD.EndsWith("C[ARSN].mtd",StringComparison.OrdinalIgnoreCase))??throw new InvalidDataException("The native template has no supported skinned C[ARSN] material");
        var material=native.Materials[template.MaterialIndex];var buffers=template.VertexBuffers.Select(b=>new FLVER2.VertexBuffer(b.LayoutIndex){BufferIndex=b.BufferIndex}).ToArray();
        var textureSource=textures.GroupBy(t=>t.Name,StringComparer.OrdinalIgnoreCase).ToDictionary(g=>g.Key,g=>g.First(),StringComparer.OrdinalIgnoreCase);var used=new HashSet<string>(StringComparer.OrdinalIgnoreCase);var normalTextures=new HashSet<string>(StringComparer.OrdinalIgnoreCase);var colorTextures=new HashSet<string>(StringComparer.OrdinalIgnoreCase);var fallbacks=new List<string>();
        bool packedShininess=sourceProfile is "DS1R" or "DS2" or "DS2S";
        var result=FLVER2.Read(nativeBytes);result.Meshes.Clear();result.Materials.Clear();
        var low=new Vector3(float.MaxValue);var high=new Vector3(float.MinValue);double exact=0,total=0;int vertices=0,triangles=0;
        foreach(var mesh in source.Meshes){
            if(mesh.MaterialIndex<0 || mesh.MaterialIndex>=source.Materials.Count)throw new InvalidDataException("Invalid source material index");var src=source.Materials[mesh.MaterialIndex];
            var mask=Regex.Match(src.Name,@"#(\d+)#");if(masks!=null && mask.Success && !masks.Contains(int.Parse(mask.Groups[1].Value)))continue;
            var mat=new FLVER2.Material("Imported_"+result.Materials.Count,CharacterMtd(src.MTD,material.MTD),material.Index){GXIndex=material.GXIndex,Textures=material.Textures.Select(Copy).ToList()};
            foreach(var texture in mat.Textures){
                // Remastered/DS2 g_Specular is packed shininess, not native RGB
                // reflectance (the same distinction is made by Smithbox).
                if(packedShininess&&texture.ParamName=="g_SpecularTexture") {
                    texture.Path=@"N:\SPRJ\data\Other\SysTex\SYSTEX_DummyReflectance.tga";
                    fallbacks.Add(src.Name+": "+sourceProfile+" packed g_Specular -> shininess; native neutral reflectance approximation");continue;
                }
                var from=src.Textures.FirstOrDefault(t=>TextureSlot(t.ParamName)==TextureSlot(texture.ParamName));
                if(packedShininess&&texture.ParamName=="g_ShininessTexture")from??=src.Textures.FirstOrDefault(t=>TextureSlot(t.ParamName)=="g_Specular");
                var name=from==null?"":TextureName(from.Path);
                if(sourceProfile=="DS1R"&&texture.ParamName=="g_DiffuseTexture"&&textureSource.TryGetValue(name,out var colorSource)) {
                    name+="_srgb";textureSource.TryAdd(name,colorSource);colorTextures.Add(name);
                }
                if(textureSource.ContainsKey(name)){texture.Path=@"N:\SPRJ\data\Model\chr\"+textureOwner+@"\tex\"+OwnedTexture(name)+".tif";used.Add(name);if(texture.ParamName=="g_BumpmapTexture")normalTextures.Add(name);}
                else if(texture.ParamName=="g_DiffuseTexture")throw new InvalidDataException("Source diffuse texture is unavailable: "+name);
                else if(texture.ParamName is "g_SpecularTexture" or "g_ShininessTexture" or "g_BumpmapTexture"){
                    var fallback=texture.ParamName=="g_BumpmapTexture"?"__bbimport_normal":texture.ParamName=="g_SpecularTexture"?"__bbimport_black":"__bbimport_white";
                    if(!textureSource.ContainsKey(fallback)){var pixels=fallback=="__bbimport_normal"?new byte[]{128,128,255,255}:fallback=="__bbimport_black"?new byte[]{0,0,0,255}:new byte[]{255,255,255,255};textureSource[fallback]=new TPF.Texture(fallback,9,0,RgbaTexture(pixels),TPF.TPFPlatform.PC);}
                    texture.Path=@"N:\SPRJ\data\Model\chr\"+textureOwner+@"\tex\"+OwnedTexture(fallback)+".tif";used.Add(fallback);fallbacks.Add(src.Name+": "+texture.ParamName+" / "+name+" -> "+fallback);
                }
            }
            var output=new FLVER2.Mesh{UseBoneWeights=true,MaterialIndex=result.Materials.Count,NodeIndex=-1,VertexBuffers=buffers.Select(b=>new FLVER2.VertexBuffer(b.LayoutIndex){BufferIndex=b.BufferIndex}).ToList()};result.Materials.Add(mat);
            foreach(var vertex in mesh.Vertices){
                var v=new FLVER.Vertex(vertex);if(!Finite(v.Position) || !Finite(v.Normal))throw new InvalidDataException("Non-finite source vertex");
                var weights=new Dictionary<int,double>();var position=Vector3.Zero;var normal=Vector3.Zero;var tangent=new Vector3[vertex.Tangents.Count];double sum=0;
                for(int n=0;n<(mesh.Dynamic==1?4:1);++n){
                    var weight=mesh.Dynamic==1?vertex.BoneWeights[n]:1f;if(!float.IsFinite(weight) || weight<0)throw new InvalidDataException("Invalid skin weight");if(weight==0)continue;
                    var index=mesh.Dynamic==1?vertex.BoneIndices[n]:mesh is FLVER0.Mesh?vertex.BoneIndices[0]:vertex.NormalW;
                    var bone=Bone(mesh,index);var target=Target(bone);var original=mesh.Dynamic==1?vertex.Position:Vector3.Transform(vertex.Position,sw[bone]);
                    position+=Vector3.Transform(original,changed[bone])*weight;
                    var normalTransform=normals[bone];var tangentTransform=changed[bone];
                    if(mesh.Dynamic!=1){if(!Matrix4x4.Invert(sw[bone],out var invWorld))throw new InvalidDataException("Singular rigid source bind pose");normalTransform=Matrix4x4.Transpose(invWorld)*normalTransform;tangentTransform=sw[bone]*tangentTransform;}
                    normal+=Vector3.TransformNormal(vertex.Normal,normalTransform)*weight;
                    for(int t=0;t<tangent.Length;++t){var p=new Vector3(vertex.Tangents[t].X,vertex.Tangents[t].Y,vertex.Tangents[t].Z);tangent[t]+=Vector3.TransformNormal(p,tangentTransform)*weight;}
                    weights[target]=weights.GetValueOrDefault(target)+weight;sum+=weight;total+=weight;if(direct[bone])exact+=weight;
                }
                if(sum<.0001 || weights.Count>4)throw new InvalidDataException("Vertex has no usable skin weights");v.Position=position/(float)sum;
                if(!Finite(v.Position) || normal.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid retargeted vertex");v.Normal=Vector3.Normalize(normal);v.NormalW=0;v.BoneIndices=new();v.BoneWeights=new();
                var ordered=weights.OrderByDescending(p=>p.Value).ToArray();for(int n=0;n<ordered.Length;++n){v.BoneIndices[n]=ordered[n].Key;v.BoneWeights[n]=(float)(ordered[n].Value/sum);}
                for(int t=0;t<tangent.Length;++t){var xyz=tangent[t]-v.Normal*Vector3.Dot(v.Normal,tangent[t]);if(xyz.LengthSquared()<1e-12f)xyz=Vector3.Cross(v.Normal,Math.Abs(v.Normal.X)<.9f?Vector3.UnitX:Vector3.UnitY);v.Tangents[t]=new Vector4(Vector3.Normalize(xyz),vertex.Tangents[t].W);}
                while(v.Tangents.Count<2)v.Tangents.Add(new Vector4(1,0,0,1));while(v.UVs.Count<2)v.UVs.Add(Vector3.Zero);while(v.Colors.Count<1)v.Colors.Add(new FLVER.VertexColor(1f,1f,1f,1f));
                output.Vertices.Add(v);low=Vector3.Min(low,v.Position);high=Vector3.Max(high,v.Position);++vertices;
            }
            IEnumerable<(FLVER2.FaceSet.FSFlags flags,bool cull,short unknown,List<int> indices)> faces=mesh is FLVER2.Mesh f?
                f.FaceSets.Where(s=>(s.Flags&FLVER2.FaceSet.FSFlags.EdgeCompressed)==0).Select(s=>(s.Flags,s.CullBackfaces,s.Unk06,s.Triangulate(f.Vertices.Count<65535))):
                new[]{(FLVER2.FaceSet.FSFlags.None,((FLVER0.Mesh)mesh).CullBackfaces,(short)0,((FLVER0.Mesh)mesh).Triangulate(((FLVER0)source).Header.Version,false,true))};
            foreach(var (flags,cull,unknown,indices) in faces){if(indices.Count%3!=0 || indices.Any(i=>i<0 || i>=output.Vertices.Count))throw new InvalidDataException("Invalid source triangle index");output.FaceSets.Add(new FLVER2.FaceSet(flags,false,cull,unknown,indices));if(flags==FLVER2.FaceSet.FSFlags.None)triangles+=indices.Count/3;}
            if(output.FaceSets.Count==0)throw new InvalidDataException("Source mesh contains no supported faces");output.BoundingBox=new FLVER2.Mesh.BoundingBoxes{Min=output.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=output.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};result.Meshes.Add(output);
        }
        if(vertices==0 || triangles==0)throw new InvalidDataException("The selected render mask has no geometry");if(total==0 || exact/total<.5)throw new InvalidDataException("Less than half the skin weight has a direct bone match. Choose a compatible template or supply a bone mapping.");
        result.Header.BoundingBoxMin=low;result.Header.BoundingBoxMax=high;
        var tpf=new TPF{Platform=TPF.TPFPlatform.PS4,Encoding=1,Flag2=3};long textureBytes=0;
        foreach(var name in used.Order(StringComparer.OrdinalIgnoreCase)){
            var sourceTexture=textureSource[name];var dds=sourceTexture.HeaderizeExt(out var extension);if(extension!=".dds")throw new InvalidDataException("Unsupported source texture container: "+extension);
            var header=new DDS(dds);if(header.dwWidth<1 || header.dwHeight<1 || header.dwWidth>8192 || header.dwHeight>8192 || dds.Length>128*1024*1024)throw new InvalidDataException("Source texture exceeds import limits");
            if(packedShininess&&normalTextures.Contains(name)&&header.GetDXGIFormat()==DDS.DXGI_FORMAT.BC5_UNORM) {
                dds=NativeBc5Normal(dds);header=new DDS(dds);fallbacks.Add(name+": BC5 normal RG retained; native gloss modulation B=255");
            }
            if(colorTextures.Contains(name)){dds=NativeSrgbColor(dds);header=new DDS(dds);}
            var texture=NativeTexture(OwnedTexture(name),dds);textureBytes+=texture.Bytes.Length;if(textureBytes>256*1024*1024)throw new InvalidDataException("Converted textures exceed import budget");
            // A PS4 write/read must preserve every compressed image and mip payload.
            var before=DdsMipImages(dds);var round=texture.HeaderizeExt(out _);List<Headerizer.Image> after;
            try{after=DdsMipImages(round);}catch(Exception e){throw new InvalidDataException("PS4 texture payload cannot be read: "+name+" (source format "+sourceTexture.Format+", DXGI "+texture.Header.DXGIFormat+")",e);}
            if(before.Count!=after.Count || before.Where((image,i)=>image.subImages.Count!=after[i].subImages.Count || image.subImages.Where((mip,m)=>!mip.SequenceEqual(after[i].subImages[m])).Any()).Any())throw new InvalidDataException("PS4 texture round-trip verification failed: "+name);
            tpf.Textures.Add(texture);
        }
        var encoded=result.Write();var checkedModel=FLVER2.Read(encoded);if(checkedModel.Meshes.Count!=result.Meshes.Count || checkedModel.Meshes.Sum(m=>m.Vertices.Count)!=vertices)throw new InvalidDataException("Converted FLVER round-trip verification failed");
        var bones=new List<BoneMatch>();for(int i=0;i<mapping.Length;++i)if(ready[i])bones.Add(new(source.Nodes[i].Name,native.Nodes[mapping[i]].Name,direct[i]));return new(result,tpf,bones,vertices,triangles,exact/total,fallbacks);
    }
}
