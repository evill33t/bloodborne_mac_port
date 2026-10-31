// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text;
static class CollisionTests {
    static void Check(bool ok,string why){if(!ok)throw new Exception(why);}
    static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Malformed input accepted");}
    static void Main(string[] args){
        MapReplacementTests.Run();
        MapAreaNameTests.Run();
        MapEnvironmentTests.Run();
        if(args.Length>1&&Directory.Exists(args[1]))MapEnvironmentTests.RunAssets(args[0],args[1]);
        MapCollisionBindingsTests.Run();
        MapConnectionsTests.Run();
        MapRuntimeLinksTests.Run();
        SourceFlverTests.Run(args.Length>1?args[1]:null);
        SourceCollisionTests.Run();
        LegacySourceCollisionTests.Run();
        NpcAppearanceTests.Run();
        StaticMapGeometryTests.Run();
        string path=Path.Combine(args[0],"dvdroot_ps4/map/m24_01_00_00/h24_01_00_00.hkx");using var archive=new BXF4Reader(path+"bhd",path+"bdt");var template=archive.ReadFile(archive.Files.Single(f=>f.Name.EndsWith("000010.hkx.dcx"))).ToArray();var before=System.Security.Cryptography.SHA256.HashData(template);
        var triangles=new List<MapCollision.Triangle>();for(int x=0;x<10;++x)for(int z=0;z<10;++z){triangles.Add(new(new(x,0,z),new(x,0,z+1),new(x+1,0,z)));triangles.Add(new(new(x+1,0,z),new(x,0,z+1),new(x+1,0,z+1)));}
        var b=MapCollision.Bake(template,triangles);uint U(int o)=>BitConverter.ToUInt32(b,o);int start=BitConverter.ToInt16(b,62)+64,names=(int)U(start+20),h=start+128,data=(int)U(h+20),local=(int)U(h+24),global=(int)U(h+28),virt=(int)U(h+32),end=(int)U(h+36);var fix=new Dictionary<int,int>();for(int o=data+local;o+8<=data+global;o+=8)if(U(o)!=uint.MaxValue)fix.Add((int)U(o),(int)U(o+4));
        var classes=new Dictionary<string,int>();for(int o=data+virt;o+12<=data+end;o+=12)if(U(o)!=uint.MaxValue){int n=names+(int)U(o+8);classes[Encoding.ASCII.GetString(b,n,Array.IndexOf(b,(byte)0,n)-n)]=(int)U(o);}
        int shape=classes["fsnpCustomParamCompressedMeshShape"],tree=classes["hknpCompressedMeshShapeData"]+16;
        Check(U(data+shape+40)==uint.MaxValue,"Empty welding map must use the disabled sentinel");Check(!fix.ContainsKey(shape+48)&&!fix.ContainsKey(shape+64),"Disabled welding arrays must have no fixups");
        int at=data+fix[tree],count=(int)U(data+tree+8),sections=(int)U(data+tree+72);Check(sections>1&&count==2*sections-1,"Fixture must exercise an internal collision tree");var nodes=new List<HKX2.hkcdStaticTreeCodec3Axis5>();
        // Read node bytes with Smithbox's independent codec, including its high-byte-first index.
        for(int i=0;i<count;++i){var node=new HKX2.hkcdStaticTreeCodec3Axis5();node.Read(new HKX2.PackFileDeserializer(),new BinaryReaderEx(false,b.AsMemory(at+i*5,5)));nodes.Add(node);}
        Check((nodes[0].m_hiData&128)!=0,"Root must decode as a branch in the native codec");var seen=new HashSet<int>();
        void Walk(int index){Check(index>=0&&index<nodes.Count,"Tree child index out of range");var n=nodes[index];if((n.m_hiData&128)!=0){Walk(index+1);Walk(index+(((n.m_hiData&127)<<8)|n.m_loData)*2);}else{int section=(n.m_hiData<<8)|n.m_loData;Check(section<sections&&seen.Add(section),"Invalid/duplicate section leaf");int sectionAt=data+fix[tree+64]+section*96;Check(BitConverter.ToUInt16(b,sectionAt+90)==index,"Section's reverse leaf reference is wrong");Check((U(sectionAt+76)&255)==b[sectionAt+88],"Packed vertex cutoff must match the packed vertex count");}}
        Walk(0);Check(seen.Count==sections,"Tree omitted sections");
        int keyBits=(int)U(data+tree+52),keyCount=(int)U(data+shape+168);var decodedKeys=new HashSet<uint>();
        Check(keyCount==triangles.Count,"A generated triangle lost its native shape key");
        for(int i=0;i<keyCount;i++) {
            uint key=U(data+fix[shape+160]+i*4)>>(32-keyBits);int sectionIndex=(int)(key>>8),primitiveIndex=(int)(key&255)>>1;
            Check((key&1)==0&&decodedKeys.Add(key),"A triangular primitive uses a quad's second triangle or a duplicate key");
            Check(sectionIndex<sections,"Shape key addresses a nonexistent collision section");
            int sectionAt=data+fix[tree+64]+sectionIndex*96,primitiveCount=(int)(U(sectionAt+80)&255),firstPrimitive=(int)(U(sectionAt+80)>>8);
            Check(primitiveIndex<primitiveCount,"Shape key addresses a nonexistent primitive");
            int primitive=data+fix[tree+80]+(firstPrimitive+primitiveIndex)*4;
            Check(b[primitive+2]==b[primitive+3],"A generated triangular primitive became a quad");
        }
        Check(decodedKeys.Max()==U(data+tree+56),"Collision key width/range does not describe its actual triangles");
        var singleSection=MapCollision.Bake(template,triangles.Take(2).ToArray());
        Check(BitConverter.ToUInt32(singleSection,data+tree+52)==2&&BitConverter.ToUInt32(singleSection,data+tree+56)==2,"Two triangular primitives require native keys 0 and 2, including two key bits");
        Check(U(data+tree+104)>0&&U(data+tree+136)>0,"Joined floor sections need a shared vertex pool");
        var sharedUses=new Dictionary<ushort,int>();
        for(int s=0;s<sections;s++) {
            int sectionAt=data+fix[tree+64]+s*96,firstShared=(int)(U(sectionAt+76)>>8),sharedCount=b[sectionAt+89];
            for(int v=0;v<sharedCount;v++){ushort index=BitConverter.ToUInt16(b,data+fix[tree+96]+(firstShared+v)*2);Check(index<U(data+tree+136),"Shared vertex index is outside its pool");sharedUses[index]=sharedUses.GetValueOrDefault(index)+1;}
        }
        Check(sharedUses.Values.All(n=>n>1),"A shared floor vertex was independently quantized at a section seam");
        var groups=MapCollision.Partition(triangles,7);Check(groups.Count==7&&groups.Sum(g=>g.Count)==triangles.Count,"Partition lost triangles");
        Check(MapCollision.RenameBody(b,"h000010","h600000").Length==b.Length,"Body rename changed fixup offsets");Reject(()=>MapCollision.RenameBody(b,"h000010","bad"));Reject(()=>MapCollision.Bake(template,[]));Reject(()=>MapCollision.Bake(new byte[1],triangles));Reject(()=>MapCollision.Bake(template,[new(new(float.NaN,0,0),Vector3.Zero,Vector3.One)]));
        Check(before.SequenceEqual(System.Security.Cryptography.SHA256.HashData(template)),"Native template was modified");Check(MapCollision.Translate(b,Vector3.Zero).SequenceEqual(b),"Zero body translation changed bytes");
        QuantizedBounds(template);
        MaterialOverrides(template);
        Console.WriteLine("PASS: native codec reads multi-section BVH; welding sentinel, reverse leaf indices, vertex cutoffs, partitions, body names and invalid inputs verified");
    }

    static void MaterialOverrides(byte[] template) {
        // Scramble separate floor patches so section sorting cannot accidentally
        // leave the source material list in input order. Decode the emitted
        // position to identify each patch independently of exporter ordering.
        var triangles=Enumerable.Range(0,192).OrderBy(i=>(i*73)%193).Select(i=>
            new MapCollision.Triangle(new(i*2,0,0),new(i*2,0,1),new(i*2+1,0,0),i%4==0?103u:i%4==1?101u:null)).ToArray();
        var bytes=MapCollision.Bake(template,triangles);
        Check(PackingHash(bytes)==PackingHash(MapCollision.Bake(template,triangles.Select(t=>t with{NativeMaterial=null}).ToArray())),"Material preservation changed encoded geometry");
        var (data,tree,fix)=Layout(bytes);uint U(int at)=>BitConverter.ToUInt32(bytes,data+at);
        int section=BitConverter.ToInt16(bytes,62)+64,names=(int)BitConverter.ToUInt32(bytes,section+20),header=section+128;
        int virt=BitConverter.ToInt32(bytes,header+32),end=BitConverter.ToInt32(bytes,header+36),param=-1,shape=-1;
        for(int at=data+virt;at<data+end;at+=12)if(BitConverter.ToUInt32(bytes,at)!=uint.MaxValue) {
            int name=names+BitConverter.ToInt32(bytes,at+8);string type=Encoding.ASCII.GetString(bytes,name,Array.IndexOf(bytes,(byte)0,name)-name);
            if(type=="fsnpCustomMeshParameter")param=BitConverter.ToInt32(bytes,at);
            if(type=="fsnpCustomParamCompressedMeshShape")shape=BitConverter.ToInt32(bytes,at);
        }
        Check(param>=0&&shape>=0,"Missing physical material metadata");
        int rows=fix[param+16],primitives=fix[param+32],primitiveCount=(int)U(param+40),keyBits=(int)U(tree+52),checkedRows=0;
        uint defaultIndex=U(rows+triangles.Length*20);Check(defaultIndex<primitiveCount,"Invalid default primitive index");
        uint defaultMaterial=U(primitives+(int)defaultIndex*56+48);
        for(int s=0;s<U(tree+72);s++) {
            int sec=fix[tree+64]+s*96,first=(int)(U(sec+80)>>8),count=(int)(U(sec+80)&255),packedFirst=(int)U(sec+72),packedCount=(int)(U(sec+76)&255);
            for(int i=0;i<count;i++) {
                int row=first+i;uint key=U(fix[shape+160]+row*4)>>(32-keyBits);
                Check(key==(uint)((s<<8)|(i<<1)),"Material rows do not follow native zero-based shape-key lookup");
                int index=bytes[data+fix[tree+80]+row*4];Check(index<packedCount,"Separated floor fixture unexpectedly needs shared vertices");
                uint word=U(fix[tree+112]+(packedFirst+index)*4);
                float x=(word&2047)*BitConverter.ToSingle(bytes,data+sec+60)+BitConverter.ToSingle(bytes,data+sec+48);
                int patch=(int)MathF.Round(x/2);uint expected=patch%4==0?103u:patch%4==1?101u:defaultMaterial;
                uint physical=U(rows+row*20);Check(physical<primitiveCount&&U(primitives+(int)physical*56+48)==expected,"Physical material attached to the wrong triangle after BVH sorting");
                if(expected!=defaultMaterial) {
                    int donor=primitives+(int)defaultIndex*56,clone=primitives+(int)physical*56;
                    Check(bytes.AsSpan(data+donor,48).SequenceEqual(bytes.AsSpan(data+clone,48))&&U(donor+52)==U(clone+52),"Material clone changed native contact data");
                    for(int a=0;a<48;a+=16)Check(fix.GetValueOrDefault(donor+a,-1)==fix.GetValueOrDefault(clone+a,-1),"Material clone lost nested native array fixups");
                }
                checkedRows++;
            }
        }
        Check(checkedRows==triangles.Length,"Material verification omitted triangles");
        Console.WriteLine("PASS: mixed stair/floor materials follow native shape keys through section sorting; geometry, default materials and nested native data preserved");
    }

    static void QuantizedBounds(byte[] template) {
        // Long, shallow triangles make the 11/11/10-bit packing error exceed
        // the old 1mm padding. Interior vertices pack locally; joined section
        // edges exercise the independent 21/21/22-bit shared vertex pool.
        var input=new List<MapCollision.Triangle>();
        Vector3 Point(int x,int z)=>new(-200+x*13.371f,-40+x*.0037f+z*.029113f,70+z*113.153f);
        for(int x=0;x<12;x++)for(int z=0;z<8;z++){input.Add(new(Point(x,z),Point(x,z+1),Point(x+1,z)));input.Add(new(Point(x+1,z),Point(x,z+1),Point(x+1,z+1)));}
        var bytes=MapCollision.Bake(template,input);
        VerifyBounds(bytes,input.Count);
        Check(PackingHash(bytes)=="E2D14F01FB18EEA833FFC9A33E82E5B87ADB39977957B46C6655BD54522D2043", "BVH correction changed collision vertex encoding or its packing grids");
        Console.WriteLine("PASS: quantized packed/shared vertices fit every section and top-level BVH ancestor; vertex encoding and packing domains preserved");
    }

    // Decode the emitted bytes independently, using double precision for the
    // enclosure audit. The 0.05mm tolerance only covers float codec roundoff.
    internal static void VerifyBounds(byte[] bytes,int expectedTriangles) {
        var (data,tree,fix)=Layout(bytes);uint U(int at)=>BitConverter.ToUInt32(bytes,data+at);
        double[] Vec(int at)=>Enumerable.Range(0,3).Select(i=>(double)BitConverter.ToSingle(bytes,data+at+i*4)).ToArray();
        var domainLo=Vec(tree+16);var domainHi=Vec(tree+32);int sections=(int)U(tree+72),verticesChecked=0;
        Check(sections>1&&U(tree+120)>0&&U(tree+136)>0,"Enclosure fixture must exercise packed and shared vertices in multiple sections");
        var sectionVertices=new List<double[]>[sections];
        void Contains(double[] lo,double[] hi,IEnumerable<double[]> vertices,string where){foreach(var v in vertices)for(int a=0;a<3;a++)Check(v[a]>=lo[a]-.00005&&v[a]<=hi[a]+.00005,$"Decoded vertex escapes {where} axis{a}: {v[a]} outside {lo[a]}..{hi[a]}");}
        List<double[]> Walk(int nodes,int count,int index,double[] lo,double[] hi,bool top,Func<int,List<double[]>> leaf,HashSet<int> seen) {
            Check(index>=0&&index<count&&seen.Add(index),"Invalid or repeated BVH node");int at=data+nodes+index*(top?5:4);var nlo=new double[3];var nhi=new double[3];
            for(int a=0;a<3;a++){byte code=bytes[at+a];double unit=(hi[a]-lo[a])/226;nlo[a]=lo[a]+(code>>4)*(code>>4)*unit;nhi[a]=hi[a]-(code&15)*(code&15)*unit;}
            int codeIndex=top?(bytes[at+3]<<8)|bytes[at+4]:bytes[at+3];bool branch=top?(codeIndex&32768)!=0:(codeIndex&1)!=0;List<double[]> points;
            if(branch){int right=top?(codeIndex&32767)*2:codeIndex&254;points=Walk(nodes,count,index+1,nlo,nhi,top,leaf,seen);points.AddRange(Walk(nodes,count,index+right,nlo,nhi,top,leaf,seen));}
            else points=new(leaf(top?codeIndex:codeIndex/2));
            Contains(nlo,nhi,points,top?"top-level ancestor":"section ancestor");return points;
        }
        for(int s=0;s<sections;s++) {
            int section=fix[tree+64]+s*96,packedStart=(int)U(section+72),sharedStart=(int)(U(section+76)>>8),packedCount=(int)(U(section+76)&255),firstPrimitive=(int)(U(section+80)>>8),primitiveCount=(int)(U(section+80)&255);
            var offset=Vec(section+48);var scale=Vec(section+60);var triangles=new List<double[]>[primitiveCount];
            double[] Vertex(int index) {
                if(index<packedCount){uint word=U(fix[tree+112]+(packedStart+index)*4);return new[]{(word&2047)*scale[0]+offset[0],((word>>11)&2047)*scale[1]+offset[1],(word>>22)*scale[2]+offset[2]};}
                int at=data+fix[tree+96]+(sharedStart+index-packedCount)*2;int shared=BitConverter.ToUInt16(bytes,at);Check(shared<U(tree+136),"Invalid shared vertex index");ulong value=BitConverter.ToUInt64(bytes,data+fix[tree+128]+shared*8);
                return new[]{(value&2097151)*(domainHi[0]-domainLo[0])/2097151+domainLo[0],((value>>21)&2097151)*(domainHi[1]-domainLo[1])/2097151+domainLo[1],(value>>42)*(domainHi[2]-domainLo[2])/4194303+domainLo[2]};
            }
            for(int i=0;i<primitiveCount;i++){int at=data+fix[tree+80]+(firstPrimitive+i)*4;Check(bytes[at+2]==bytes[at+3],"Fixture triangle became a quad");triangles[i]=Enumerable.Range(0,3).Select(v=>Vertex(bytes[at+v])).ToList();verticesChecked+=3;}
            var seen=new HashSet<int>();sectionVertices[s]=Walk(fix[section],(int)U(section+8),0,Vec(section+16),Vec(section+32),false,i=>triangles[i],seen);Check(seen.Count==U(section+8),"Section BVH omitted nodes");
        }
        var visited=new HashSet<int>();var all=Walk(fix[tree],(int)U(tree+8),0,domainLo,domainHi,true,i=>sectionVertices[i],visited);Contains(domainLo,domainHi,all,"shared vertex domain");
        Check(visited.Count==U(tree+8)&&verticesChecked==expectedTriangles*3&&all.Count==verticesChecked,"BVH omitted or duplicated triangle vertices");
    }

    internal static string PackingHash(byte[] bytes) {
        var (data,tree,fix)=Layout(bytes);using var stream=new MemoryStream();void Copy(int at,int count)=>stream.Write(bytes,data+at,count);
        Copy(tree+16,32);int sections=BitConverter.ToInt32(bytes,data+tree+72);for(int s=0;s<sections;s++)Copy(fix[tree+64]+s*96+48,42);
        foreach(var (at,stride) in new[]{(tree+80,4),(tree+96,2),(tree+112,4),(tree+128,8)}){int count=BitConverter.ToInt32(bytes,data+at+8);if(count>0)Copy(fix[at],count*stride);}
        return Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(stream.ToArray()));
    }

    static (int Data,int Tree,Dictionary<int,int> Fix) Layout(byte[] bytes) {
        uint U(int at)=>BitConverter.ToUInt32(bytes,at);int section=BitConverter.ToInt16(bytes,62)+64,names=(int)U(section+20),header=section+128,data=(int)U(header+20),local=(int)U(header+24),global=(int)U(header+28),virt=(int)U(header+32),end=(int)U(header+36),tree=-1;
        var fix=new Dictionary<int,int>();for(int at=data+local;at<data+global;at+=8)if(U(at)!=uint.MaxValue)fix.Add((int)U(at),(int)U(at+4));
        for(int at=data+virt;at<data+end;at+=12)if(U(at)!=uint.MaxValue){int name=names+(int)U(at+8);if(Encoding.ASCII.GetString(bytes,name,Array.IndexOf(bytes,(byte)0,name)-name)=="hknpCompressedMeshShapeData")tree=(int)U(at)+16;}
        Check(tree>=0,"Missing emitted collision tree");return(data,tree,fix);
    }
}
