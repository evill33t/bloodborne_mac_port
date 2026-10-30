// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;
using System.Numerics;
using System.Security.Cryptography;
using System.Runtime.CompilerServices;

// Bounded DSR FaceGen material adapter. The source shader blends two UV sets
// using vertex alpha before its gamma2.2 conversion; neither texture alpha is
// its blend mask. These operations do not reproduce source subsurface lighting.
internal static class NpcFaceMaterial {
    internal sealed record Report(string TemplateSHA256,string Diffuse,string Specular,string Normal,
        int Width,int Height,Vector3 SkinAddColor,int CoveredPixels,int DilatedPixels,float MaximumOverlapRGBError,IReadOnlyList<string> Limitations);
    sealed record Identity(string SHA256);
    static readonly ConditionalWeakTable<FLVER2,Identity> VerifiedTemplates=new();
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static string Stem(string path)=>Path.GetFileNameWithoutExtension(Leaf(path));
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);

    internal static FLVER2 ReadTemplate(byte[] original) {
        if(original.Length is <128 or >1024*1024||!original.AsSpan(0,8).SequenceEqual("FLVER\0L\0"u8))throw new InvalidDataException("Unsupported FaceGen template container");
        var bytes=original.ToArray();
        int I(int at){if(at<0||at>bytes.Length-4)throw new InvalidDataException("Truncated FaceGen template");return BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(at,4));}
        void Set(int at,int value)=>BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(at,4),value);
        if(I(8)!=0x2000C||I(20)!=0||I(24)!=4||I(28)!=7||I(32)!=4||I(36)!=4||I(80)!=24||I(84)!=4)throw new InvalidDataException("FaceGen template table contract changed");
        int data=I(12),buffers=128+I(24)*32+I(28)*128+I(32)*48+I(80)*32,layouts=buffers+4*32;
        if(data<layouts+4*16||data>bytes.Length)throw new InvalidDataException("FaceGen tables exceed metadata");
        int[] counts=[642,332,78,4],strides=[48,48,36,40],sourceLayouts=[1,1,2,2];
        for(int i=0;i<4;i++) {
            int at=buffers+i*32,offset=I(at+28);long end=(long)data+offset+(long)counts[i]*strides[i];
            if(I(at)!=0x40000000||I(at+4)!=sourceLayouts[i]||I(at+8)!=strides[i]||I(at+12)!=counts[i]||I(at+16)!=0||I(at+20)!=0||I(at+24)!=counts[i]*strides[i]||offset<0||end>bytes.Length)
                throw new InvalidDataException("Unsupported FaceGen vertex-buffer encoding");
        }
        int[][] faceLayout=[[0,0,2,0,0],[0,12,17,2,0],[0,16,26,1,0],[0,24,19,3,0],[0,28,19,6,0],[0,32,19,7,0],[0,36,19,10,0],[0,40,22,5,0]];
        int[][] eyeLayout=[[0,0,2,0,0],[0,12,17,2,0],[0,16,26,1,0],[0,24,19,3,0],[0,28,19,6,0],[0,32,19,10,0],[0,36,21,5,0]];
        void Layout(int index,int[][] expected) {
            int at=layouts+index*16,start=I(at+12);
            if(I(at)!=expected.Length||start<layouts+64||start>data-expected.Length*20)throw new InvalidDataException("Unsupported FaceGen vertex layout");
            for(int m=0;m<expected.Length;m++)for(int k=0;k<5;k++)if(I(start+m*20+k*4)!=expected[m][k])throw new InvalidDataException("FaceGen vertex-layout member changed");
        }
        Layout(1,faceLayout);Layout(2,eyeLayout);
        // The auxiliary40-byte card shares the Eye's declared layout. Give the
        // real36-byte Eye its own unused layout before calling SoulsFormats,
        // whose DSR fix would otherwise mutate that shared declaration.
        int free=I(layouts+12),freeCount=I(layouts),source=I(layouts+2*16+12);
        if(freeCount<6||free<layouts+64||free>data-freeCount*20||Enumerable.Range(0,4).Any(i=>I(buffers+i*32+4)==0))throw new InvalidDataException("FaceGen private Eye layout slot is unavailable");
        int targetMember=0;
        for(int member=0;member<eyeLayout.Length;member++)if(member!=4) {
            bytes.AsSpan(source+member*20,20).CopyTo(bytes.AsSpan(free+targetMember*20,20));
            Set(free+targetMember*20+4,member>=5?eyeLayout[member][1]-4:eyeLayout[member][1]);++targetMember;
        }
        Set(layouts,6);Set(buffers+2*32+4,0);
        for(int i=0;i<3;i++)Set(buffers+i*32,0);
        var result=FLVER2.Read(bytes);
        if(result.Meshes.Count!=4)throw new InvalidDataException("FaceGen decoded mesh count changed");
        for(int m=0;m<3;m++) {
            if(result.Meshes[m].Vertices.Count!=counts[m])throw new InvalidDataException("FaceGen decoded vertex count changed");
            int begin=data+I(buffers+m*32+28),stride=strides[m],color=m<2?36:28,uv=m<2?40:32;
            for(int i=0;i<counts[m];i++) {
                int at=begin+i*stride;var v=result.Meshes[m].Vertices[i];
                if(!Finite(v.Position)||!Finite(v.Normal)||v.UVs.Count!=(m<2?2:1)||v.Colors.Count!=1)throw new InvalidDataException("FaceGen attributes were not decoded");
                var c=v.Colors[0];
                if(Math.Abs(c.R-original[at+color]/255f)>1e-6f||Math.Abs(c.G-original[at+color+1]/255f)>1e-6f||Math.Abs(c.B-original[at+color+2]/255f)>1e-6f||Math.Abs(c.A-original[at+color+3]/255f)>1e-6f)throw new InvalidDataException("FaceGen vertex colors failed raw verification");
                for(int u=0;u<v.UVs.Count;u++) {
                    float x=BinaryPrimitives.ReadInt16LittleEndian(original.AsSpan(at+uv+u*4,2))/1024f,y=BinaryPrimitives.ReadInt16LittleEndian(original.AsSpan(at+uv+u*4+2,2))/1024f;
                    if(v.UVs[u]!=new Vector3(x,y,0))throw new InvalidDataException("FaceGen UVs failed raw verification");
                }
            }
        }
        VerifiedTemplates.Add(result,new(Convert.ToHexString(SHA256.HashData(original)).ToLowerInvariant()));return result;
    }

    internal static Report Bake(FLVER2 generated,FLVER2 template,List<TPF.Texture> textures,string generatedEgtName,byte[] meanBmp,int faceID,int sex) {
        if(sex is not(0 or 1)||faceID<0||generated.Meshes.Count!=3||template.Meshes.Count!=4||!VerifiedTemplates.TryGetValue(template,out var identity))throw new InvalidDataException("Face atlas requires a verified source FaceGen template");
        TPF.Texture Find(string name) {
            var found=textures.Where(t=>Stem(t.Name).Equals(Stem(name),StringComparison.OrdinalIgnoreCase)).ToArray();
            return found.Length==1?found[0]:throw new InvalidDataException("Missing or ambiguous FaceGen atlas texture: "+name);
        }
        FLVER2.Texture Binding(FLVER2.Material material,string parameter) {
            var found=material.Textures.Where(t=>t.ParamName==parameter).ToArray();
            if(found.Length!=1||found[0].TilingScale!=Vector2.One)throw new InvalidDataException("Unsupported FaceGen material binding: "+parameter);return found[0];
        }
        var baseColor=Decode(Find(generatedEgtName));if(baseColor.Width!=128||baseColor.Height!=128)throw new InvalidDataException("Unexpected generated FaceGen color size");
        var skinAdd=SkinAdd(baseColor,meanBmp);
        var first=generated.Materials[generated.Meshes[0].MaterialIndex];
        var detailBinding=Binding(first,"g_Diffuse_2");string detailName=detailBinding.Path;
        if(sex==0)detailName=detailName.Replace("FG_m_face_org_d","FG_f_face_org_d",StringComparison.OrdinalIgnoreCase);
        string baseSpecName=Binding(first,"g_Specular").Path,detailSpecName=Binding(first,"g_Specular_2").Path,detailNormalName=Binding(first,"g_Bumpmap_2").Path;
        var detail=Decode(Find(detailName));var baseSpec=Decode(Find(baseSpecName));var detailSpec=Decode(Find(detailSpecName));var detailNormal=Decode(Find(detailNormalName),nativeNormal:true);
        if(detail.Width!=512||detail.Height!=512)throw new InvalidDataException("Source FaceGen detail atlas resolution changed");
        const int width=2048,height=512;
        string diffuseName=$"FG_import_{faceID}_face_composite",specularName=$"FG_import_{faceID}_face_specular",normalName=$"FG_import_{faceID}_face_detail_normal";
        if(textures.Any(t=>new[]{diffuseName,specularName,normalName}.Contains(Stem(t.Name),StringComparer.OrdinalIgnoreCase)))throw new InvalidDataException("FaceGen owned atlas texture already exists");
        var colorPixels=new byte[width*height*4];var specPixels=new byte[colorPixels.Length];var normalPixels=new byte[colorPixels.Length];
        var covered=new bool[width*height];var quality=Enumerable.Repeat(float.NegativeInfinity,covered.Length).ToArray();float maximumOverlap=0;
        var atlasUV=new Vector3[3][];var secondUV=new Vector3[3][];var tangents=new Vector4[3][];
        for(int m=0;m<3;m++) {
            var source=template.Meshes[m];var target=generated.Meshes[m];
            if(source.Vertices.Count!=target.Vertices.Count||target.Vertices.Count is <1 or >10000)throw new InvalidDataException("Generated FaceGen topology does not match template");
            atlasUV[m]=new Vector3[target.Vertices.Count];secondUV[m]=new Vector3[target.Vertices.Count];
            for(int v=0;v<target.Vertices.Count;v++) {
                var a=source.Vertices[v];var b=target.Vertices[v];
                if(a.UVs.Count!=(m<2?2:1)||a.Colors.Count!=1||b.UVs.Count!=2||b.Colors.Count!=1||!Finite(b.Position)||!Finite(b.Normal))throw new InvalidDataException("FaceGen atlas vertex contract changed");
                var uv=a.UVs[0];atlasUV[m][v]=m<2?new(uv.X/4,uv.Y+2f/512,0):uv;secondUV[m][v]=m<2?a.UVs[1]:uv;
                if(atlasUV[m][v].X<0||atlasUV[m][v].X>1||atlasUV[m][v].Y<0||atlasUV[m][v].Y>1)throw new InvalidDataException("FaceGen atlas UV exceeds packed chart");
            }
            tangents[m]=Tangents(target,atlasUV[m]);if(m==2)continue;
            var material=generated.Materials[target.MaterialIndex];
            if(!Stem(Binding(material,"g_Diffuse").Path).Equals(Stem(generatedEgtName),StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("FaceGen generated albedo binding differs from atlas input");
            _=Binding(material,"g_Bumpmap");
            if(!Stem(Binding(material,"g_Diffuse_2").Path).Equals(Stem(detailBinding.Path),StringComparison.OrdinalIgnoreCase)||
                !Stem(Binding(material,"g_Specular").Path).Equals(Stem(baseSpecName),StringComparison.OrdinalIgnoreCase)||
                !Stem(Binding(material,"g_Specular_2").Path).Equals(Stem(detailSpecName),StringComparison.OrdinalIgnoreCase)||
                !Stem(Binding(material,"g_Bumpmap_2").Path).Equals(Stem(detailNormalName),StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("FaceGen face subsets have different material inputs");
            var indices=source.FaceSets.Single(f=>f.Flags==FLVER2.FaceSet.FSFlags.None).Triangulate(source.Vertices.Count<65535);
            var generatedIndices=target.FaceSets.Single(f=>f.Flags==FLVER2.FaceSet.FSFlags.None).Triangulate(target.Vertices.Count<65535);
            if(!indices.SequenceEqual(generatedIndices)||indices.Count%3!=0||indices.Any(v=>v<0||v>=source.Vertices.Count))throw new InvalidDataException("FaceGen atlas facets differ from source");
            for(int f=0;f<indices.Count;f+=3) {
                int ia=indices[f],ib=indices[f+1],ic=indices[f+2];var a=source.Vertices[ia];var b=source.Vertices[ib];var c=source.Vertices[ic];
                Vector2 Point(int index)=>new(atlasUV[m][index].X*width,atlasUV[m][index].Y*height);
                var pa=Point(ia);var pb=Point(ib);var pc=Point(ic);float denominator=(pb.Y-pc.Y)*(pa.X-pc.X)+(pc.X-pb.X)*(pa.Y-pc.Y);if(Math.Abs(denominator)<1e-8f)continue;
                int xmin=Math.Max(0,(int)MathF.Floor(Math.Min(pa.X,Math.Min(pb.X,pc.X)))),xmax=Math.Min(width-1,(int)MathF.Ceiling(Math.Max(pa.X,Math.Max(pb.X,pc.X))));
                int ymin=Math.Max(0,(int)MathF.Floor(Math.Min(pa.Y,Math.Min(pb.Y,pc.Y)))),ymax=Math.Min(height-1,(int)MathF.Ceiling(Math.Max(pa.Y,Math.Max(pb.Y,pc.Y))));
                for(int y=ymin;y<=ymax;y++)for(int x=xmin;x<=xmax;x++) {
                    float u=x+.5f,v=y+.5f,wa=((pb.Y-pc.Y)*(u-pc.X)+(pc.X-pb.X)*(v-pc.Y))/denominator,wb=((pc.Y-pa.Y)*(u-pc.X)+(pa.X-pc.X)*(v-pc.Y))/denominator,wc=1-wa-wb;
                    float margin=Math.Min(wa,Math.Min(wb,wc));if(margin< -1e-6f)continue;
                    Vector2 UV(int set)=>new(a.UVs[set].X*wa+b.UVs[set].X*wb+c.UVs[set].X*wc,a.UVs[set].Y*wa+b.UVs[set].Y*wb+c.UVs[set].Y*wc);
                    var uv0=UV(0);var uv1=UV(1);float alpha=a.Colors[0].A*wa+b.Colors[0].A*wb+c.Colors[0].A*wc;
                    var rgb=new Vector3(a.Colors[0].R*wa+b.Colors[0].R*wb+c.Colors[0].R*wc,a.Colors[0].G*wa+b.Colors[0].G*wb+c.Colors[0].G*wc,a.Colors[0].B*wa+b.Colors[0].B*wb+c.Colors[0].B*wc);
                    var sourceBase=baseColor.Sample(uv0);var sourceDetail=detail.Sample(uv1);var color=Vector3.Lerp(new(sourceBase.X,sourceBase.Y,sourceBase.Z),new Vector3(sourceDetail.X,sourceDetail.Y,sourceDetail.Z)+skinAdd,alpha)*rgb;
                    var spec=Vector4.Lerp(baseSpec.Sample(uv0),detailSpec.Sample(uv1),alpha);var normal=detailNormal.Sample(uv1);
                    int pixel=y*width+x,at=pixel*4;
                    if(covered[pixel])maximumOverlap=Math.Max(maximumOverlap,Math.Max(Math.Abs(colorPixels[at]/255f-color.X),Math.Max(Math.Abs(colorPixels[at+1]/255f-color.Y),Math.Abs(colorPixels[at+2]/255f-color.Z))));
                    if(covered[pixel]&&margin<=quality[pixel])continue;
                    covered[pixel]=true;quality[pixel]=margin;
                    colorPixels[at]=Channel(color.X);colorPixels[at+1]=Channel(color.Y);colorPixels[at+2]=Channel(color.Z);colorPixels[at+3]=255;
                    specPixels[at]=Channel(spec.X);specPixels[at+1]=Channel(spec.Y);specPixels[at+2]=Channel(spec.Z);specPixels[at+3]=Channel(spec.W);
                    normalPixels[at]=Channel(normal.X);normalPixels[at+1]=Channel(normal.Y);normalPixels[at+2]=Channel(normal.Z);normalPixels[at+3]=Channel(normal.W);
                }
            }
        }
        int pixels=covered.Count(v=>v);if(pixels<200000)throw new InvalidDataException("Insufficient FaceGen atlas coverage");
        Dilate([colorPixels,specPixels,normalPixels],covered,width,height);
        var additions=new[]{new TPF.Texture(diffuseName,9,0,Texture(colorPixels,width,height),TPF.TPFPlatform.PC),new TPF.Texture(specularName,9,0,Texture(specPixels,width,height),TPF.TPFPlatform.PC),new TPF.Texture(normalName,9,0,Texture(normalPixels,width,height),TPF.TPFPlatform.PC)};
        // Commit only after all source validation, rasterization and DDS writes
        // succeed. Geometry, weights, skeleton and every LOD index remain intact.
        for(int m=0;m<3;m++) {
            var mesh=generated.Meshes[m];
            for(int v=0;v<mesh.Vertices.Count;v++) {
                var vertex=mesh.Vertices[v];vertex.UVs[0]=atlasUV[m][v];vertex.UVs[1]=secondUV[m][v];vertex.Colors[0]=new FLVER.VertexColor(1f,1f,1f,1f);
                for(int t=0;t<vertex.Tangents.Count;t++)vertex.Tangents[t]=tangents[m][v];
            }
            if(m==2)continue;var material=generated.Materials[mesh.MaterialIndex];
            Binding(material,"g_Diffuse").Path=diffuseName+".dds";Binding(material,"g_Specular").Path=specularName+".dds";Binding(material,"g_Bumpmap").Path=normalName+".dds";
        }
        textures.AddRange(additions);
        return new(identity.SHA256,diffuseName,specularName,normalName,width,height,skinAdd,pixels,width*height-pixels,maximumOverlap,[
            "Face albedo/specular blend uses source authored UVs, vertex-alpha interpolation and encoded-space skin offset. Native material color power, BRDF, gamma and subsurface lighting remain separate.",
            "Source detail UVs overlap mirrored face halves, so the output uses the authored base-UV chart at512 texels/unit in a2048x512 atlas. Quantized source UV seam overlaps select the deepest triangle interior; localized atlas rasterization is approximate.",
            "Normals sample only the source detail normal map through its authored UV1. Source dual-normal mixing and conversion between its two tangent frames are not reproduced."
        ]);
    }

    sealed record Image(int Width,int Height,byte[] Pixels) {
        internal Vector4 Sample(Vector2 uv) {
            if(!float.IsFinite(uv.X)||!float.IsFinite(uv.Y)||Math.Abs(uv.X)>16||Math.Abs(uv.Y)>16)throw new InvalidDataException("FaceGen texture coordinate exceeds budget");
            double x=uv.X*Width-.5,y=uv.Y*Height-.5;int ix=(int)Math.Floor(x),iy=(int)Math.Floor(y);float fx=(float)(x-ix),fy=(float)(y-iy);
            Vector4 At(int a,int b){a=(a%Width+Width)%Width;b=(b%Height+Height)%Height;int p=(b*Width+a)*4;return new Vector4(Pixels[p],Pixels[p+1],Pixels[p+2],Pixels[p+3])/255f;}
            return Vector4.Lerp(Vector4.Lerp(At(ix,iy),At(ix+1,iy),fx),Vector4.Lerp(At(ix,iy+1),At(ix+1,iy+1),fx),fy);
        }
    }
    static Image Decode(TPF.Texture texture,bool nativeNormal=false) {
        byte[] dds=texture.HeaderizeExt(out var extension);if(extension!=".dds"||dds.Length>32*1024*1024)throw new InvalidDataException("Unsupported FaceGen texture container");
        // BC5 has no blue channel. Retain the native converter's neutral gloss
        // modulation when resampling into an RGBA atlas, rather than baking B0.
        if(nativeNormal&&new DDS(dds).GetDXGIFormat()==DDS.DXGI_FORMAT.BC5_UNORM)dds=SkinRetarget.NativeBc5Normal(dds);
        using var image=Pfim.Pfimage.FromStream(new MemoryStream(dds));
        if(image.Width is <1 or >2048||image.Height is <1 or >2048||image.Format is not(Pfim.ImageFormat.Rgba32 or Pfim.ImageFormat.Rgb24))throw new InvalidDataException("Unsupported FaceGen texture pixels");
        int step=image.BitsPerPixel/8;var result=new byte[image.Width*image.Height*4];
        for(int y=0;y<image.Height;y++)for(int x=0;x<image.Width;x++) {
            int p=y*image.Stride+x*step,q=(y*image.Width+x)*4;result[q]=image.Data[p+2];result[q+1]=image.Data[p+1];result[q+2]=image.Data[p];result[q+3]=step==4?image.Data[p+3]:(byte)255;
        }
        return new(image.Width,image.Height,result);
    }
    static Vector3 SkinAdd(Image generated,byte[] bmp) {
        if(bmp.Length<54||bmp[0]!='B'||bmp[1]!='M'||BitConverter.ToInt32(bmp,18)!=generated.Width||BitConverter.ToInt32(bmp,22)!=generated.Height||BitConverter.ToUInt16(bmp,28)!=24||BitConverter.ToInt32(bmp,30)!=0)throw new InvalidDataException("Unsupported FaceGen neutral mean BMP");
        int offset=BitConverter.ToInt32(bmp,10),stride=(generated.Width*3+3)/4*4;long end=(long)offset+stride*generated.Height;
        if(offset<54||end>bmp.Length)throw new InvalidDataException("Truncated FaceGen neutral mean BMP");
        int at=offset+(generated.Height-1)*stride+(generated.Width-1)*3,p=(generated.Width-1)*4;
        return new Vector3(generated.Pixels[p]-bmp[at+2],generated.Pixels[p+1]-bmp[at+1],generated.Pixels[p+2]-bmp[at])/255f;
    }
    static byte Channel(float value)=>!float.IsFinite(value)?throw new InvalidDataException("Nonfinite FaceGen atlas color"):(byte)Math.Clamp((int)MathF.Round(value*255),0,255);
    static byte[] Texture(byte[] rgba,int width,int height) {
        var levels=new List<byte[]>{rgba};int w=width,h=height;
        while(w>1||h>1) {
            int nw=Math.Max(1,w/2),nh=Math.Max(1,h/2);var previous=levels[^1];var next=new byte[nw*nh*4];
            for(int y=0;y<nh;y++)for(int x=0;x<nw;x++)for(int c=0;c<4;c++) {
                int sum=0;for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++)sum+=previous[(Math.Min(h-1,y*2+dy)*w+Math.Min(w-1,x*2+dx))*4+c];next[(y*nw+x)*4+c]=(byte)((sum+2)/4);
            }
            levels.Add(next);w=nw;h=nh;
        }
        var header=new DDS{dwWidth=width,dwHeight=height,dwMipMapCount=levels.Count,dwPitchOrLinearSize=width*4};
        header.dwFlags|=DDS.DDSD.PITCH|DDS.DDSD.MIPMAPCOUNT;header.dwCaps|=DDS.DDSCAPS.COMPLEX|DDS.DDSCAPS.MIPMAP;
        header.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;header.ddspf.dwRGBBitCount=32;header.ddspf.dwRBitMask=0xff;header.ddspf.dwGBitMask=0xff00;header.ddspf.dwBBitMask=0xff0000;header.ddspf.dwABitMask=0xff000000;
        return header.Write(levels.SelectMany(p=>p).ToArray());
    }
    static void Dilate(byte[][] images,bool[] covered,int width,int height) {
        var queue=new Queue<int>();var used=covered.ToArray();for(int i=0;i<used.Length;i++)if(used[i])queue.Enqueue(i);
        if(queue.Count==0)throw new InvalidDataException("FaceGen atlas has no raster coverage");
        while(queue.TryDequeue(out int at)) {
            int x=at%width,y=at/width;
            foreach(int next in new[]{x>0?at-1:-1,x+1<width?at+1:-1,y>0?at-width:-1,y+1<height?at+width:-1})if(next>=0&&!used[next]) {
                used[next]=true;foreach(var image in images)image.AsSpan(at*4,4).CopyTo(image.AsSpan(next*4,4));queue.Enqueue(next);
            }
        }
    }
    static Vector4[] Tangents(FLVER2.Mesh mesh,IReadOnlyList<Vector3> uv) {
        var tangents=new Vector3[mesh.Vertices.Count];var bitangents=new Vector3[tangents.Length];
        var faces=mesh.FaceSets.Single(f=>f.Flags==FLVER2.FaceSet.FSFlags.None).Triangulate(mesh.Vertices.Count<65535);
        for(int f=0;f<faces.Count;f+=3) {
            int a=faces[f],b=faces[f+1],c=faces[f+2];var e1=mesh.Vertices[b].Position-mesh.Vertices[a].Position;var e2=mesh.Vertices[c].Position-mesh.Vertices[a].Position;
            var u1=uv[b]-uv[a];var u2=uv[c]-uv[a];float determinant=u1.X*u2.Y-u1.Y*u2.X;if(Math.Abs(determinant)<1e-12f)continue;
            var t=(e1*u2.Y-e2*u1.Y)/determinant;var bt=(e2*u1.X-e1*u2.X)/determinant;foreach(int v in new[]{a,b,c}){tangents[v]+=t;bitangents[v]+=bt;}
        }
        var result=new Vector4[tangents.Length];
        for(int v=0;v<result.Length;v++) {
            var n=mesh.Vertices[v].Normal;var t=tangents[v]-n*Vector3.Dot(n,tangents[v]);if(t.LengthSquared()<1e-12f)t=Vector3.Cross(n,Math.Abs(n.X)<.9f?Vector3.UnitX:Vector3.UnitY);
            if(!Finite(t)||t.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid FaceGen atlas tangent");t=Vector3.Normalize(t);result[v]=new(t,Vector3.Dot(Vector3.Cross(n,t),bitangents[v])<0?-1:1);
        }
        return result;
    }
}
