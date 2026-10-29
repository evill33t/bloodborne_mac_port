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
        var groups=MapCollision.Partition(triangles,7);Check(groups.Count==7&&groups.Sum(g=>g.Count)==triangles.Count,"Partition lost triangles");
        Check(MapCollision.RenameBody(b,"h000010","h600000").Length==b.Length,"Body rename changed fixup offsets");Reject(()=>MapCollision.RenameBody(b,"h000010","bad"));Reject(()=>MapCollision.Bake(template,[]));Reject(()=>MapCollision.Bake(new byte[1],triangles));Reject(()=>MapCollision.Bake(template,[new(new(float.NaN,0,0),Vector3.Zero,Vector3.One)]));
        Check(before.SequenceEqual(System.Security.Cryptography.SHA256.HashData(template)),"Native template was modified");Check(MapCollision.Translate(b,Vector3.Zero).SequenceEqual(b),"Zero body translation changed bytes");
        Console.WriteLine("PASS: native codec reads multi-section BVH; welding sentinel, reverse leaf indices, vertex cutoffs, partitions, body names and invalid inputs verified");
    }
}
