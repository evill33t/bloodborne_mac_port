// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Globalization;
using System.Numerics;
using System.Text;

internal record NpcFaceResult(FLVER2 Model,List<FLVER2> SourceParts,List<TPF.Texture> Textures,
    List<string> SourcePaths,List<string> Limitations,int FaceGenID,int HairStyle);

// FaceGen's public TRI/EGM/EGT layouts are documented by Singular Inversions:
// https://facegen.com/dl/sdk/doc/manual/fileformats.html
// DS1R executable a45aaa36...98d7b, 0x140771c50, decodes each saved byte as
// float(byte)*20/255-10. 0x140777f90 accumulates EGT modes in 16.16 fixed point.
// The shipped FG FLVER buffers are empty runtime templates, not neutral heads.
internal static class NpcFace {
    sealed record Polygon(int[] Vertices,int[] UVs);
    sealed record Shape(Vector3[] Positions,Vector2[] UVs,List<Polygon> Polygons);
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static Memory<byte> Asset(BND3 binder,string name) {
        var files=binder.Files.Where(f=>Leaf(f.Name).Equals(name,StringComparison.OrdinalIgnoreCase)).ToArray();
        if(files.Length!=1)throw new InvalidDataException("Missing or ambiguous FaceGen asset: "+name);return files[0].Bytes;
    }
    static byte[] Required(Func<string,byte[]?> read,string path)=>read(path)??throw new FileNotFoundException("NPC face asset unavailable",path);
    static BinaryReader Reader(Memory<byte> bytes,string magic) {
        if(bytes.Length<64||bytes.Length>32*1024*1024)throw new InvalidDataException("FaceGen asset exceeds read budget");
        var reader=new BinaryReader(new MemoryStream(bytes.ToArray(),false));
        if(Encoding.ASCII.GetString(reader.ReadBytes(8))!=magic)throw new InvalidDataException("Unexpected FaceGen format: "+magic);return reader;
    }
    static float Float(BinaryReader reader) {
        float f=reader.ReadSingle();return float.IsFinite(f)&&Math.Abs(f)<1000000?f:throw new InvalidDataException("Invalid FaceGen coordinate");
    }
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z)&&p.LengthSquared()<1000000;
    static int Cell(PARAM.Row row,string name) {
        var cell=row.Cells?.SingleOrDefault(c=>c.Def.InternalName==name)??throw new InvalidDataException("Missing FaceGen field: "+name);
        int n=Convert.ToInt32(cell.Value,CultureInfo.InvariantCulture);return n is >=0 and <=255?n:throw new InvalidDataException("FaceGen value outside byte range");
    }
    internal static float Coefficient(int value)=>value is >=0 and <=255?value*20f/255f-10f:throw new ArgumentOutOfRangeException(nameof(value));
    internal static int HairModel(int style,int sex) {
        if(style is <0 or >19||sex is not(0 or 1))throw new InvalidDataException("Unsupported DS1 hairstyle selection");
        // EquipParamProtector rows 1000+100*style (male), 3000+100*style
        // (female) define these resource IDs. The selection is not a filename.
        int[] models=sex==1?[0,2,11,4,12,6,10,5,7,3]:[0,6,2,4,5,7,8,3,9,10];return style<models.Length?models[style]:0;
    }
    static Shape Tri(Memory<byte> bytes) {
        using var r=Reader(bytes,"FRTRI003");int count=r.ReadInt32(),triangles=r.ReadInt32(),quads=r.ReadInt32();
        if(count is <1 or >10000||triangles<0||quads<0||triangles+quads>30000)throw new InvalidDataException("Invalid FaceGen TRI size");
        if(r.ReadInt32()!=0||r.ReadInt32()!=0)throw new NotSupportedException("Labelled FaceGen TRI is unsupported");
        int uvs=r.ReadInt32(),extensions=r.ReadInt32();
        if(uvs is <1 or >30000||extensions!=1||r.ReadInt32()!=0||r.ReadInt32()!=0||r.ReadInt32()!=0)throw new NotSupportedException("Unsupported FaceGen TRI morph/UV representation");
        r.BaseStream.Position=64;
        var p=new Vector3[count];for(int i=0;i<count;++i)p[i]=new(Float(r),Float(r),Float(r));
        var polygons=new List<Polygon>();
        for(int i=0;i<triangles+quads;++i) {
            int n=i<triangles?3:4;var indices=new int[n];for(int j=0;j<n;++j){indices[j]=r.ReadInt32();if(indices[j]<0||indices[j]>=count)throw new InvalidDataException("Invalid FaceGen TRI vertex");}
            polygons.Add(new(indices,new int[n]));
        }
        var uv=new Vector2[uvs];for(int i=0;i<uv.Length;++i)uv[i]=new(Float(r),1-Float(r));
        foreach(var poly in polygons)for(int i=0;i<poly.UVs.Length;++i){int index=r.ReadInt32();if(index<0||index>=uv.Length)throw new InvalidDataException("Invalid FaceGen TRI UV");poly.UVs[i]=index;}
        if(r.BaseStream.Position!=r.BaseStream.Length)throw new InvalidDataException("Unexpected FaceGen TRI trailing data");return new(p,uv,polygons);
    }
    static Vector3[] Morph(Shape shape,Memory<byte> bytes,float[] coefficients) {
        using var r=Reader(bytes,"FREGM002");int count=r.ReadInt32(),symmetric=r.ReadInt32(),asymmetric=r.ReadInt32();uint basis=r.ReadUInt32();
        if(count!=shape.Positions.Length||symmetric!=50||asymmetric!=30||basis!=2001060901||bytes.Length!=64+(4+count*6)*80)throw new InvalidDataException("FaceGen EGM basis or dimensions do not match");
        var positions=shape.Positions.ToArray();r.BaseStream.Position=64;
        for(int mode=0;mode<symmetric;++mode) {
            float scale=Float(r)*coefficients[mode];
            for(int v=0;v<count;++v)positions[v]+=new Vector3(r.ReadInt16(),r.ReadInt16(),r.ReadInt16())*scale;
        }
        // DS1's saved FaceGen row contains no asymmetric coefficients.
        if(positions.Any(p=>!Finite(p)))throw new InvalidDataException("Nonfinite morphed FaceGen position");return positions;
    }
    static (int,int,int) Key(int a,int b,int c) {if(a>b)(a,b)=(b,a);if(b>c)(b,c)=(c,b);if(a>b)(a,b)=(b,a);return(a,b,c);}
    // FaceGen's TRI/EGM basis has the opposite Z direction from source FLVER.
    // The forty generated outer-rim points match the authored FC_M_0000 skull
    // opening to <1 micrometre after this reflection (X/Y already agree).
    // Reflect normals too, and retain the original FLVER triangle winding.
    static Vector3 FlverBasis(Vector3 point)=>new(point.X,point.Y,-point.Z);
    static FLVER2.BufferLayout Layout()=>new() {
        new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Position),new(FLVER.LayoutType.UByte4,FLVER.LayoutSemantic.BoneIndices),
        new(FLVER.LayoutType.Short4Norm,FLVER.LayoutSemantic.BoneWeights),new(FLVER.LayoutType.Float3,FLVER.LayoutSemantic.Normal),
        new(FLVER.LayoutType.Float4,FLVER.LayoutSemantic.Tangent,0),new(FLVER.LayoutType.Float4,FLVER.LayoutSemantic.Tangent,1),
        // Packed UVs avoid the installed reader/writer's asymmetric Float2 scaling.
        new(FLVER.LayoutType.Short4,FLVER.LayoutSemantic.UV,0),
        new(FLVER.LayoutType.UByte4Norm,FLVER.LayoutSemantic.VertexColor)
    };
    static FLVER2 Face(FLVER2 template,FLVER2 canonical,F2TR mappings,Shape face,Shape eye,Vector3[] facePoints,Vector3[] eyePoints,int id,int sex,string diffuse) {
        if(template.Meshes.Count!=4||mappings.Entries.Count!=4||canonical.Nodes.Count is <1 or >256)throw new InvalidDataException("Unexpected FaceGen FLVER template");
        int sourceHead=template.Nodes.FindIndex(n=>n.Name=="Head"),head=canonical.Nodes.FindIndex(n=>n.Name=="Head");
        if(sourceHead<0||head<0)throw new InvalidDataException("FaceGen source or canonical Head bone unavailable");
        var sw=SkinRetarget.World(template.Nodes)[sourceHead];var cw=SkinRetarget.World(canonical.Nodes)[head];
        if(Vector3.Distance(sw.Translation,cw.Translation)>.001f||new[]{Vector3.UnitX,Vector3.UnitY,Vector3.UnitZ}.Any(v=>Vector3.Distance(Vector3.TransformNormal(v,sw),Vector3.TransformNormal(v,cw))>.001f))throw new InvalidDataException("FaceGen and c0000 Head bind frames do not match");
        var output=new FLVER2();output.Header.Version=0x2000C;output.Header.Unicode=true;output.Nodes=canonical.Nodes.Select(n=>new FLVER.Node(n){Flags=n.Flags}).ToList();output.BufferLayouts.Add(Layout());
        for(int m=0;m<3;++m) {
            var shape=m==2?eye:face;var points=m==2?eyePoints:facePoints;var map=mappings.Entries[m];var from=template.Meshes[m];
            if(map.Name!=(m==2?"eye.tri":"face.tribe")||map.Indices.Count!=from.Vertices.Count||map.Indices.Any(v=>v<0||v>=points.Length))throw new InvalidDataException("FaceGen F2TR vertex mapping does not match FLVER");
            var polyByTriangle=new Dictionary<(int,int,int),Polygon>();var fallbackUv=new Dictionary<int,Vector2>();var normals=new Vector3[points.Length];
            foreach(var poly in shape.Polygons) {
                for(int a=0;a<poly.Vertices.Length;++a)fallbackUv.TryAdd(poly.Vertices[a],shape.UVs[poly.UVs[a]]);
                for(int a=0;a<poly.Vertices.Length-2;++a)for(int b=a+1;b<poly.Vertices.Length-1;++b)for(int c=b+1;c<poly.Vertices.Length;++c)
                    if(!polyByTriangle.TryAdd(Key(poly.Vertices[a],poly.Vertices[b],poly.Vertices[c]),poly))throw new InvalidDataException("Ambiguous FaceGen TRI facet");
                for(int p=1;p<poly.Vertices.Length-1;++p) {
                    int a=poly.Vertices[0],b=poly.Vertices[p],c=poly.Vertices[p+1];var normal=Vector3.Cross(points[b]-points[a],points[c]-points[a]);
                    normals[a]+=normal;normals[b]+=normal;normals[c]+=normal;
                }
            }
            var sourceMaterial=template.Materials[from.MaterialIndex];
            var material=new FLVER2.Material($"#12# FaceGen_{id}_{m}",sourceMaterial.MTD,sourceMaterial.Index){Textures=sourceMaterial.Textures.Select(t=>new FLVER2.Texture(t.ParamName,t.Path,t.TilingScale,t.TilingTypeU,t.TilingTypeV,t.Unk14,t.Unk18,t.Unk1C)).ToList()};
            foreach(var t in material.Textures) {
                if(m<2&&t.ParamName=="g_Diffuse")t.Path=diffuse+".dds";
                if(sex==0&&t.ParamName=="g_Bumpmap"&&Leaf(t.Path).Equals("FG_m_face_n.tga",StringComparison.OrdinalIgnoreCase))t.Path="FG_f_face_n.tga";
            }
            var mesh=new FLVER2.Mesh{UseBoneWeights=true,NodeIndex=-1,MaterialIndex=output.Materials.Count,VertexBuffers=[new(0)]};var assigned=new bool[map.Indices.Count];var uv=new Vector2[assigned.Length];
            foreach(var fs in from.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None)) {
                var indices=fs.Triangulate(from.Vertices.Count<65535);
                for(int f=0;f<indices.Count;f+=3) {
                    int a=indices[f],b=indices[f+1],c=indices[f+2];
                    if(a<0||b<0||c<0||a>=assigned.Length||b>=assigned.Length||c>=assigned.Length)throw new InvalidDataException("Invalid FaceGen FLVER facet");
                    if(!polyByTriangle.TryGetValue(Key(map.Indices[a],map.Indices[b],map.Indices[c]),out var poly))throw new InvalidDataException("FaceGen FLVER facet has no source TRI UVs");
                    foreach(int v in new[]{a,b,c}) {
                        var value=shape.UVs[poly.UVs[Array.IndexOf(poly.Vertices,map.Indices[v])]];
                        if(assigned[v]&&Vector2.Distance(uv[v],value)>.00001f)throw new InvalidDataException("Conflicting FaceGen seam UVs");uv[v]=value;assigned[v]=true;
                    }
                }
                mesh.FaceSets.Add(new(fs.Flags,false,fs.CullBackfaces,fs.Unk06,indices));
            }
            // FaceGen's runtime template contains the same six draw variants as
            // the authored skull/hair: base/LOD1/LOD2 and their motion-blur sets.
            // Keep their topology so selecting a lower LOD cannot hide the face.
            // Their collapsed triangles do not necessarily occur in the TRI, so
            // recover seam UVs from the base topology above, then reuse vertices.
            foreach(var fs in from.FaceSets.Where(f=>f.Flags!=FLVER2.FaceSet.FSFlags.None)) {
                if((fs.Flags&FLVER2.FaceSet.FSFlags.EdgeCompressed)!=0)throw new NotSupportedException("Compressed FaceGen draw topology is unsupported");
                var indices=fs.Triangulate(from.Vertices.Count<65535);
                if(indices.Count%3!=0||indices.Any(v=>v<0||v>=assigned.Length))throw new InvalidDataException("Invalid FaceGen draw-variant facet");
                mesh.FaceSets.Add(new(fs.Flags,false,fs.CullBackfaces,fs.Unk06,indices));
            }
            for(int v=0;v<assigned.Length;++v) {
                int original=map.Indices[v];var normal=normals[original];
                if(!Finite(normal)||normal.LengthSquared()<1e-12f)throw new InvalidDataException("Invalid generated FaceGen normal");
                if(!assigned[v])uv[v]=fallbackUv.GetValueOrDefault(original);
                if(MathF.Round(uv[v].X*1024)<short.MinValue||MathF.Round(uv[v].X*1024)>short.MaxValue||MathF.Round(uv[v].Y*1024)<short.MinValue||MathF.Round(uv[v].Y*1024)>short.MaxValue)
                    throw new InvalidDataException("Generated FaceGen UV exceeds the packed source layout");
                var vert=new FLVER.Vertex{Position=FlverBasis(points[original]),Normal=Vector3.Normalize(FlverBasis(normal)),UVs=[new(uv[v],0),new(uv[v],0)],Colors=[new FLVER.VertexColor(1f,1f,1f,1f)]};
                vert.BoneIndices[0]=head;vert.BoneWeights[0]=1;mesh.Vertices.Add(vert);
            }
            var tangents=new Vector3[mesh.Vertices.Count];var bitangents=new Vector3[tangents.Length];
            foreach(var fs in mesh.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None))for(int f=0;f<fs.Indices.Count;f+=3) {
                int a=fs.Indices[f],b=fs.Indices[f+1],c=fs.Indices[f+2];var e1=mesh.Vertices[b].Position-mesh.Vertices[a].Position;var e2=mesh.Vertices[c].Position-mesh.Vertices[a].Position;
                var u1=uv[b]-uv[a];var u2=uv[c]-uv[a];float determinant=u1.X*u2.Y-u1.Y*u2.X;
                if(Math.Abs(determinant)<1e-12f)continue;var t=(e1*u2.Y-e2*u1.Y)/determinant;var bt=(e2*u1.X-e1*u2.X)/determinant;
                foreach(int v in new[]{a,b,c}){tangents[v]+=t;bitangents[v]+=bt;}
            }
            for(int v=0;v<mesh.Vertices.Count;++v) {
                var vert=mesh.Vertices[v];var t=tangents[v]-vert.Normal*Vector3.Dot(vert.Normal,tangents[v]);
                if(t.LengthSquared()<1e-12f)t=Vector3.Cross(vert.Normal,Math.Abs(vert.Normal.X)<.9f?Vector3.UnitX:Vector3.UnitY);
                if(!Finite(t))throw new InvalidDataException("Invalid FaceGen tangent");t=Vector3.Normalize(t);
                float sign=Vector3.Dot(Vector3.Cross(vert.Normal,t),bitangents[v])<0?-1:1;vert.Tangents=[new(t,sign),new(t,sign)];
            }
            mesh.BoundingBox=new(){Min=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=mesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};
            output.Materials.Add(material);output.Meshes.Add(mesh);
        }
        output.Header.BoundingBoxMin=output.Meshes.Select(m=>m.BoundingBox.Min).Aggregate(Vector3.Min);output.Header.BoundingBoxMax=output.Meshes.Select(m=>m.BoundingBox.Max).Aggregate(Vector3.Max);return output;
    }
    static byte[] Color(Memory<byte> egt,Memory<byte> bitmap,float[] coefficients) {
        using var r=Reader(egt,"FREGT003");int height=r.ReadInt32(),width=r.ReadInt32(),symmetric=r.ReadInt32(),asymmetric=r.ReadInt32();uint basis=r.ReadUInt32();
        if(width!=128||height!=128||symmetric!=50||asymmetric!=0||basis!=81||egt.Length!=64+50*(4+width*height*3))throw new InvalidDataException("Unexpected FaceGen EGT basis or dimensions");
        var bmp=bitmap.ToArray();if(bmp.Length<54)throw new InvalidDataException("Truncated FaceGen mean BMP");
        int at=BitConverter.ToInt32(bmp,10);int stride=(width*3+3)/4*4;
        if(bmp[0]!='B'||bmp[1]!='M'||BitConverter.ToInt32(bmp,18)!=width||BitConverter.ToInt32(bmp,22)!=height||BitConverter.ToUInt16(bmp,28)!=24||BitConverter.ToInt32(bmp,30)!=0||at<54||at>bmp.Length-stride*height)throw new InvalidDataException("Unsupported FaceGen mean BMP");
        var accumulated=new int[width*height*3];r.BaseStream.Position=64;
        for(int mode=0;mode<symmetric;++mode) {
            int factor=checked((int)(coefficients[mode]*256)*(int)(Float(r)*256));
            for(int c=0;c<3;++c)for(int p=0;p<width*height;++p)accumulated[p*3+c]=checked(accumulated[p*3+c]+r.ReadSByte()*factor);
        }
        var rgba=new byte[width*height*4];
        for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
            int pixel=y*width+x;for(int c=0;c<3;++c)rgba[pixel*4+c]=(byte)Math.Clamp((int)(bmp[at+(height-1-y)*stride+x*3+2-c]+accumulated[pixel*3+c]/65536f),0,255);rgba[pixel*4+3]=255;
        }
        var dds=new DDS{dwWidth=width,dwHeight=height,dwMipMapCount=1,dwPitchOrLinearSize=width*4};dds.ddspf.dwFlags=DDS.DDPF.RGB|DDS.DDPF.ALPHAPIXELS;dds.ddspf.dwRGBBitCount=32;dds.ddspf.dwRBitMask=255;dds.ddspf.dwGBitMask=65280;dds.ddspf.dwBBitMask=16711680;dds.ddspf.dwABitMask=4278190080;return dds.Write(rgba);
    }
    internal static NpcFaceResult Load(Func<string,byte[]?> read,PARAM.Row faceGen,int sex) {
        if(sex is not(0 or 1))throw new InvalidDataException("Invalid source NPC face sex");
        var files=BND3.Read(Required(read,"facegen/FaceGen.fgbnd"));var canonicalFiles=BND3.Read(Required(read,"chr/c0000.chrbnd.dcx"));
        var canonical=FLVER2.Read(Asset(canonicalFiles,"c0000.flver"));var template=FLVER2.Read(Asset(files,"face.flver"));var mapping=F2TR.Read(Asset(files,"face.flver2tri"));
        var face=Tri(Asset(files,"face.tri"));var eye=Tri(Asset(files,"eye.tri"));
        var geo=Enumerable.Range(0,50).Select(i=>Coefficient(Cell(faceGen,$"faceGeoData{i:D2}"))).ToArray();
        var tex=Enumerable.Range(0,50).Select(i=>Coefficient(Cell(faceGen,$"faceTexData{i:D2}"))).ToArray();
        string texture=$"FG_import_{faceGen.ID}_face";var generated=Face(template,canonical,mapping,face,eye,Morph(face,Asset(files,"face.egm"),geo),Morph(eye,Asset(files,"eye.egm"),geo),faceGen.ID,sex,texture);
        var textures=TPF.Read(Asset(files,"facegen.tpf")).Textures.ToList();textures.Add(new(texture,9,0,Color(Asset(files,"face.egt"),Asset(files,"face.bmp"),tex),TPF.TPFPlatform.PC));
        var sources=new List<string>{"facegen/FaceGen.fgbnd","chr/c0000.chrbnd.dcx"};var parts=new List<FLVER2>();
        string gender=sex==1?"M":"F";
        void AddPart(string path) {
            var b=BND3.Read(Required(read,path));var flvers=b.Files.Where(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).ToArray();if(flvers.Length!=1)throw new InvalidDataException("Ambiguous NPC face part FLVER");
            var model=FLVER2.Read(flvers[0].Bytes);if(model.Meshes.Count>0)parts.Add(model);sources.Add(path);
            foreach(var file in b.Files)if(TPF.Is(file.Bytes.ToArray()))textures.AddRange(TPF.Read(file.Bytes).Textures);
        }
        AddPart($"parts/FC_{gender}_0000.partsbnd.dcx");int style=Cell(faceGen,"hairStyle");
        int hair=HairModel(style,sex);AddPart($"parts/HR_{gender}_{hair:D4}.partsbnd.dcx");
        var limitations=new List<string>{
            "Source FaceGen shape coefficients, TRI topology/UVs and full EGT color modes are reconstructed; the unsupported two-triangle auxiliary eye card is omitted.",
            "The source skin-detail overlay, subsurface and eye/hair tint shaders are not reproduced by the native material conversion; hair/neck color and alpha shading require runtime comparison.",
            "Face/eyes use the verified canonical Head bind and do not reproduce source eye animation or body-shape customization."
        };
        return new(generated,parts,textures,sources,limitations,faceGen.ID,style);
    }
}
