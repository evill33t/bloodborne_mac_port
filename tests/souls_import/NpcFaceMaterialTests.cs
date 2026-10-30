// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Buffers.Binary;
using System.Numerics;
using System.Security.Cryptography;

static class NpcFaceMaterialTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static string Stem(string path)=>Path.GetFileNameWithoutExtension(path.Replace('\\','/').Split('/').Last());
    sealed record Image(int Width,int Height,int Stride,byte[] Data) {
        internal double[] Pixel(int x,int y) {
            x=(x%Width+Width)%Width;y=(y%Height+Height)%Height;int p=y*Stride+x*4;
            return [Data[p+2]/255.0,Data[p+1]/255.0,Data[p]/255.0,Data[p+3]/255.0];
        }
        internal double[] Sample(double u,double v) {
            double x=u*Width-.5,y=v*Height-.5;int ix=(int)Math.Floor(x),iy=(int)Math.Floor(y);double fx=x-ix,fy=y-iy;
            var a=Pixel(ix,iy);var b=Pixel(ix+1,iy);var c=Pixel(ix,iy+1);var d=Pixel(ix+1,iy+1);
            return Enumerable.Range(0,4).Select(k=>(a[k]*(1-fx)+b[k]*fx)*(1-fy)+(c[k]*(1-fx)+d[k]*fx)*fy).ToArray();
        }
    }
    static Image Decode(TPF.Texture texture,bool atlas=false) {
        using var image=Pfim.Pfimage.FromStream(new MemoryStream(texture.HeaderizeExt(out _)));
        Check(image.Format==Pfim.ImageFormat.Rgba32,"Face material pixel fixture requires RGBA");
        if(atlas) {
            Check(image.Width==2048&&image.Height==512&&image.MipMaps.Length==11,"Face atlas dimensions/full mip chain changed");
            int w=image.Width,h=image.Height;foreach(var mip in image.MipMaps){w=Math.Max(1,w/2);h=Math.Max(1,h/2);Check(mip.Width==w&&mip.Height==h,"Face atlas mip dimensions changed");}
            Check(w==1&&h==1,"Face atlas mip chain is incomplete");
        }
        return new(image.Width,image.Height,image.Stride,image.Data.ToArray());
    }
    sealed record Facet(FLVER.Vertex A,FLVER.Vertex B,FLVER.Vertex C) {
        internal double[]? Weights(int x,int y) {
            double ax=A.UVs[0].X*512,ay=A.UVs[0].Y*512+2,bx=B.UVs[0].X*512,by=B.UVs[0].Y*512+2,cx=C.UVs[0].X*512,cy=C.UVs[0].Y*512+2;
            double denominator=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy);if(Math.Abs(denominator)<1e-10)return null;
            double a=((by-cy)*(x+.5-cx)+(cx-bx)*(y+.5-cy))/denominator,b=((cy-ay)*(x+.5-cx)+(ax-cx)*(y+.5-cy))/denominator;
            return [a,b,1-a-b];
        }
    }
    internal static void Run(byte[] rawTemplate,NpcFaceResult result,byte[] meanBmp) {
        var inputHash=SHA256.HashData(rawTemplate);var meanHash=SHA256.HashData(meanBmp);var modelBytes=result.Model.Write();
        var textureHashes=result.Textures.Select(t=>SHA256.HashData(t.Bytes)).ToArray();
        var template=NpcFaceMaterial.ReadTemplate(rawTemplate);
        int I(int at)=>BinaryPrimitives.ReadInt32LittleEndian(rawTemplate.AsSpan(at,4));
        int data=I(12);Check(I(24)==4&&I(28)==7&&I(32)==4&&I(36)==4,"Raw FaceGen table fixture changed");
        int[] counts=[642,332,78],strides=[48,48,36];
        for(int m=0;m<3;m++)for(int i=0;i<counts[m];i++) {
            // Literal interleaved byte offsets are independent of the repaired SF layouts.
            int at=data+I(2112+m*32+28)+i*strides[m],color=m<2?36:28,uv=m<2?40:32;var vertex=template.Meshes[m].Vertices[i];
            var position=new Vector3(BitConverter.ToSingle(rawTemplate,at),BitConverter.ToSingle(rawTemplate,at+4),BitConverter.ToSingle(rawTemplate,at+8));
            Check(vertex.Position==position&&vertex.UVs.Count==(m<2?2:1),"Private template repair lost source position/UV count");
            for(int u=0;u<vertex.UVs.Count;u++) {
                var expected=new Vector3(BinaryPrimitives.ReadInt16LittleEndian(rawTemplate.AsSpan(at+uv+u*4,2))/1024f,BinaryPrimitives.ReadInt16LittleEndian(rawTemplate.AsSpan(at+uv+u*4+2,2))/1024f,0);
                Check(vertex.UVs[u]==expected,"Private template repair changed raw authored UVs");
            }
            var actual=vertex.Colors.Single();float[] channels=[actual.R,actual.G,actual.B,actual.A];
            for(int c=0;c<4;c++)Check(Math.Abs(channels[c]-rawTemplate[at+color+c]/255f)<1e-7f,"Private template repair changed raw vertex RGBA");
        }
        Check(template.Meshes[2].VertexBuffers.Single().LayoutIndex==0&&template.BufferLayouts[0].Count==6&&template.BufferLayouts[2].Count==7,"Eye repair mutated the shared auxiliary layout");
        var report=result.Material??throw new Exception("Face material conversion report missing");
        Check(report.TemplateSHA256==Convert.ToHexString(inputHash).ToLowerInvariant()&&report.Width==2048&&report.Height==512&&report.CoveredPixels>200000,"Face material provenance/coverage changed");
        foreach(var mesh in result.Model.Meshes.Take(2))foreach(var (parameter,name) in new[]{("g_Diffuse",report.Diffuse),("g_Specular",report.Specular),("g_Bumpmap",report.Normal)})
            Check(Stem(result.Model.Materials[mesh.MaterialIndex].Textures.Single(t=>t.ParamName==parameter).Path)==name,"Face material does not reference its owned atlas");
        TPF.Texture Texture(string name)=>result.Textures.Single(t=>Stem(t.Name).Equals(Stem(name),StringComparison.OrdinalIgnoreCase));
        var albedo=Decode(Texture(report.Diffuse),true);_=Decode(Texture(report.Specular),true);var normal=Decode(Texture(report.Normal),true);
        Check(Enumerable.Range(0,normal.Width*normal.Height).All(i=>normal.Data[i/normal.Width*normal.Stride+i%normal.Width*4]==255),"BC5 normal atlas lost native neutral gloss modulation");
        var encoded=FLVER2.Read(modelBytes);
        for(int m=0;m<3;m++)for(int v=0;v<counts[m];v++) {
            var source=template.Meshes[m].Vertices[v];var target=encoded.Meshes[m].Vertices[v];var uv=source.UVs[0];
            var expected=m<2?new Vector3(uv.X/4,uv.Y+2f/512,0):uv;
            Check(Vector3.Distance(target.UVs[0],expected)<.0007f&&Vector3.Distance(target.UVs[1],source.UVs[m<2?1:0])<.0007f,"Written face atlas UVs differ from authored chart");
            var white=target.Colors.Single();Check(white.A==1&&white.R==1&&white.G==1&&white.B==1,"Face atlas uses byte-one rather than normalized-white vertex RGBA");
        }
        var baseColor=Decode(Texture($"FG_import_{result.FaceGenID}_face"));var detail=Decode(Texture("FG_m_face_org_d"));
        int bmpOffset=BitConverter.ToInt32(meanBmp,10),bmpWidth=BitConverter.ToInt32(meanBmp,18),bmpHeight=BitConverter.ToInt32(meanBmp,22),bmpStride=(bmpWidth*3+3)/4*4;
        int meanTopRight=bmpOffset+(bmpHeight-1)*bmpStride+(bmpWidth-1)*3;var generatedTopRight=baseColor.Pixel(baseColor.Width-1,0);
        var skin=Enumerable.Range(0,3).Select(c=>generatedTopRight[c]-meanBmp[meanTopRight+2-c]/255.0).ToArray();
        Check(Vector3.Distance(report.SkinAddColor,new((float)skin[0],(float)skin[1],(float)skin[2]))<1e-7f,"Face skin offset uses the wrong BMP row or pixel");
        var facets=new List<Facet>();
        foreach(var mesh in template.Meshes.Take(2)) {
            var indices=mesh.FaceSets.Single(f=>f.Flags==FLVER2.FaceSet.FSFlags.None).Triangulate(mesh.Vertices.Count<65535);
            for(int f=0;f<indices.Count;f+=3)facets.Add(new(mesh.Vertices[indices[f]],mesh.Vertices[indices[f+1]],mesh.Vertices[indices[f+2]]));
        }
        int samples=0,mixed=0;var visited=new HashSet<(int,int)>();
        foreach(var facet in facets) {
            int x=(int)Math.Floor((facet.A.UVs[0].X+facet.B.UVs[0].X+facet.C.UVs[0].X)/3*512),y=(int)Math.Floor((facet.A.UVs[0].Y+facet.B.UVs[0].Y+facet.C.UVs[0].Y)/3*512+2);
            var weights=facet.Weights(x,y);if(x<0||x>=2048||y<0||y>=512||weights==null||weights.Min()<.04||!visited.Add((x,y)))continue;
            // Skip quantized chart overlaps: the selected source surface is ambiguous there.
            if(facets.Count(f=>f.Weights(x,y) is { } w&&w.Min()>1e-6)!=1)continue;
            FLVER.Vertex[] vertices=[facet.A,facet.B,facet.C];
            double UV(int set,int axis)=>Enumerable.Range(0,3).Sum(i=>(axis==0?vertices[i].UVs[set].X:vertices[i].UVs[set].Y)*weights[i]);
            double alpha=Enumerable.Range(0,3).Sum(i=>vertices[i].Colors[0].A*weights[i]);var b=baseColor.Sample(UV(0,0),UV(0,1));var d=detail.Sample(UV(1,0),UV(1,1));var pixel=albedo.Pixel(x,y);
            for(int c=0;c<3;c++) {
                double rgb=Enumerable.Range(0,3).Sum(i=>(c==0?vertices[i].Colors[0].R:c==1?vertices[i].Colors[0].G:vertices[i].Colors[0].B)*weights[i]);
                double expected=Math.Clamp(Math.Round((b[c]*(1-alpha)+(d[c]+skin[c])*alpha)*rgb*255),0,255);
                Check(Math.Abs(pixel[c]*255-expected)<=1.01,"Baked face differs from independent two-UV, vertex-alpha source shader formula");
            }
            samples++;if(alpha<.99)mixed++;
        }
        Check(samples>800&&mixed>100,"Face atlas formula test did not cover enough real mixed/interior pixels");
        // Rebuild only a private model, restoring all documented material edits
        // afterward to prove geometry, nodes, weights and every LOD are untouched.
        var copy=FLVER2.Read(modelBytes);var localTextures=result.Textures.Where(t=>!new[]{report.Diffuse,report.Specular,report.Normal}.Contains(Stem(t.Name))).ToList();
        for(int m=0;m<2;m++)foreach(var binding in copy.Materials[copy.Meshes[m].MaterialIndex].Textures)if(binding.ParamName is "g_Diffuse" or "g_Specular" or "g_Bumpmap")
            binding.Path=binding.ParamName=="g_Diffuse"?$"FG_import_{result.FaceGenID}_face.dds":template.Materials[template.Meshes[m].MaterialIndex].Textures.Single(t=>t.ParamName==binding.ParamName).Path;
        byte[] before=copy.Write();var attrs=copy.Meshes.Select(m=>m.Vertices.Select(v=>(uv:v.UVs.ToArray(),colors:v.Colors.ToArray(),tangents:v.Tangents.ToArray())).ToArray()).ToArray();var paths=copy.Materials.Select(m=>m.Textures.Select(t=>t.Path).ToArray()).ToArray();
        _=NpcFaceMaterial.Bake(copy,template,localTextures,$"FG_import_{result.FaceGenID}_face",meanBmp,result.FaceGenID,1);
        for(int m=0;m<3;m++)for(int v=0;v<counts[m];v++){var vertex=copy.Meshes[m].Vertices[v];var a=attrs[m][v];vertex.UVs.Clear();vertex.UVs.AddRange(a.uv);vertex.Colors.Clear();vertex.Colors.AddRange(a.colors);vertex.Tangents.Clear();vertex.Tangents.AddRange(a.tangents);}
        for(int m=0;m<copy.Materials.Count;m++)for(int t=0;t<copy.Materials[m].Textures.Count;t++)copy.Materials[m].Textures[t].Path=paths[m][t];
        Check(before.SequenceEqual(copy.Write()),"Face material bake changed geometry/skeleton/weights/LOD topology");
        void Reject(byte[] bad) {
            var hash=SHA256.HashData(bad);bool rejected=false;try{_=NpcFaceMaterial.ReadTemplate(bad);}catch(InvalidDataException){rejected=true;}
            Check(rejected&&hash.SequenceEqual(SHA256.HashData(bad)),"Malformed FaceGen template accepted or mutated");
        }
        Reject(rawTemplate[..128]);
        foreach((int at,int value) in new[]{(0,0),(8,0),(32,3),(2112,0),(2112+8,44),(2112+12,643),(2112+28,int.MaxValue),(2240,5),(I(2240+16+12)+8,99),(data+I(2112+28),0x7fc00000)}) {
            var bad=rawTemplate.ToArray();BinaryPrimitives.WriteInt32LittleEndian(bad.AsSpan(at,4),value);Reject(bad);
        }
        Check(inputHash.SequenceEqual(SHA256.HashData(rawTemplate))&&meanHash.SequenceEqual(SHA256.HashData(meanBmp))&&modelBytes.SequenceEqual(result.Model.Write()),"Face material tests mutated source/result geometry");
        Check(textureHashes.Select((hash,i)=>hash.SequenceEqual(SHA256.HashData(result.Textures[i].Bytes))).All(v=>v),"Face material bake mutated original source texture bytes");
    }
}
