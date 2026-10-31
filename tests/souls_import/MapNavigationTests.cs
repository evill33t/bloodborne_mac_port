// SPDX-License-Identifier: GPL-2.0-or-later
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using HKX2;
using SoulsFormats;

static class MapNavigationTests {
    static void Check(bool v,string why){if(!v)throw new Exception(why);}
    static void Reject(Action a){try{a();}catch(InvalidDataException){return;}catch(NotSupportedException){return;}throw new Exception("Invalid navigation accepted");}
    static string Hash(byte[] bytes)=>Convert.ToHexString(SHA256.HashData(bytes));
    static hkRootLevelContainer Root(byte[] bytes)=>MapNavigation.ReadRoot(bytes);
    static void Main(string[] args) {
        if(args.Length!=2)throw new ArgumentException("Supply DS1R and native dvdroot_ps4 roots");
        var hashes=new Dictionary<string,string>();
        byte[] Read(string path){var b=File.ReadAllBytes(path);hashes.TryAdd(path,Hash(b));return b;}
        byte[]? Source(string path){string p=Path.Combine(args[0],path);return File.Exists(p)?Read(p):null;}
        // Authenticate the classes against actual BB 2014 packfiles before
        // accepting the DS3-named serializer's compatible class implementation.
        string retailPath=Path.Combine(args[1],"map/m21_00_00_00/m21_00_00_00.nvmhktbnd.dcx");
        var retail=BND4.Read(Read(retailPath));var retailHkx=retail.Files[0].Bytes.ToArray();
        var native=MapNavigation.ReadMesh(retailHkx);Check(native.m_faces.Count==415,"Retail BB nav fixture changed");
        foreach(var member in retail.Files.Take(3)){NativeContract(member.Bytes.ToArray());Check(Root(member.Bytes.ToArray()).m_namedVariants.Select(v=>v.m_variant).OfType<hkcdStaticAabbTree>().Single().m_treePtr.m_nodes.Count>0,"Retail tree storage failed independent native-offset readback");}
        foreach(var obj in new IHavokObject[]{new hkRootLevelContainer(),new hkaiNavMesh(),new hkcdStaticAabbTree(),new hkcdStaticTreeDefaultTreeStorage6(),new hkaiDirectedGraphExplicitCost()}) {
            var name=Encoding.ASCII.GetBytes(obj.GetType().Name+"\0");int pos=retailHkx.AsSpan().IndexOf(name);
            Check(pos>=5&&retailHkx[pos-1]==9&&BitConverter.ToUInt32(retailHkx,pos-5)==obj.Signature,"Serializer class signature differs from retail BB: "+obj.GetType().Name);
        }
        Fixture();
        var offset=new Vector3(-143.8f,20.07f,164.63f);var reports=new List<object>();
        foreach(var (map,target) in new[]{("m10_02_00_00","m40_02_00_00"),("m10_01_00_00","m40_03_00_00")}) {
            var export=MapNavigation.Bake("DS1R",map,target,Source,offset);
            var msb=MSB1.Read(Source($"map/MapStudio/{map}.msb")!);var originals=BND3.Read(Source($"map/{map}/{map}.nvmbnd.dcx")!);
            var binder=BND4.Read(export.Files[$"map/{target}/{target}.nvmhktbnd.dcx"]);var nva=NVA.Read(export.Files[$"map/{target}/{target}.nva.dcx"]);
            Check(export.Files.Count==2&&nva.Version==NVA.NVAVersion.DarkSouls3&&nva.SectionCount==9&&nva.NavmeshInfoEntries.Version==3,"Wrong native NVA package/schema");
            Check(binder.Files.Count==msb.Parts.Navmeshes.Count&&nva.NavmeshInfoEntries.Count==binder.Files.Count&&binder.Files.Count>0,"Native streamable navmesh count differs from source placements");
            int totalFaces=0,totalGraphEdges=0;
            for(int i=0;i<binder.Files.Count;i++) {
                var part=msb.Parts.Navmeshes[i];var data=originals.Files.Single(f=>f.Name.Replace('\\','/').EndsWith('/'+part.ModelName+"A10.nvm",StringComparison.Ordinal)).Bytes.ToArray();var source=NVM.Read(data);
                var raw=binder.Files[i].Bytes.ToArray();int treeFixup=NativeContract(raw);var rr=Root(raw);var mesh=rr.m_namedVariants.Select(v=>v.m_variant).OfType<hkaiNavMesh>().Single();var graph=rr.m_namedVariants.Select(v=>v.m_variant).OfType<hkaiDirectedGraphExplicitCost>().Single();var tree=rr.m_namedVariants.Select(v=>v.m_variant).OfType<hkcdStaticAabbTree>().Single().m_treePtr;
                if(i==0){var broken=raw.ToArray();BitConverter.TryWriteBytes(broken.AsSpan(treeFixup,4),BitConverter.ToInt32(broken,treeFixup)+8);Reject(()=>Root(broken));}
                var row=nva.NavmeshInfoEntries[i];Check(row.ModelID==i&&binder.Files[i].Name.EndsWith($"n{target[1..]}_{i:D6}.hkx",StringComparison.Ordinal),"NVA ModelID does not bind native member filename");
                Check(row.NameID==(int.Parse(target.Substring(1,2))*100+int.Parse(target.Substring(4,2)))*100000+i*10&&row.Position==Vector3.Zero&&row.Scale==Vector3.One&&row.Rotation==Vector3.Zero,"NVA frame/NameID changed");
                Check(row.FaceDataIndex==totalFaces&&row.FaceCount==mesh.m_faces.Count,"NVA face data range does not match real mesh faces");
                Check(row.ConnectedNavmeshIDs.Count==0&&row.GateNodes.Count==0&&nva.ConnectorEntries.Count==0,"Unconverted cross-mesh paths were fabricated");
                var r=part.Rotation*(MathF.PI/180);var matrix=Matrix4x4.CreateScale(part.Scale)*Matrix4x4.CreateRotationX(r.X)*Matrix4x4.CreateRotationZ(r.Z)*Matrix4x4.CreateRotationY(r.Y)*Matrix4x4.CreateTranslation(part.Position+offset);
                var expected=new List<Vector3[]>();foreach(var tri in source.Triangles){var p=new[]{source.Vertices[tri.VertexIndex1],source.Vertices[tri.VertexIndex2],source.Vertices[tri.VertexIndex3]}.Select(v=>Vector3.Transform(v,matrix)).ToArray();if(Vector3.Cross(p[1]-p[0],p[2]-p[0]).LengthSquared()>=1e-12f)expected.Add(p);}
                Check(mesh.m_faces.Count==expected.Count&&mesh.m_edges.Count==expected.Count*3&&graph.m_nodes.Count==expected.Count,"Source nav faces/graph lost");
                for(int f=0;f<mesh.m_faces.Count;f++) {
                    var face=mesh.m_faces[f];Check(face.m_numEdges==3&&face.m_clusterIndex==f,"Invalid triangle/graph cluster binding");
                    for(int e=0;e<3;e++) {
                        int index=face.m_startEdgeIndex+e;var edge=mesh.m_edges[index];var v=mesh.m_vertices[edge.m_a];Check(Vector3.Distance(expected[f][e],new(v.X,v.Y,v.Z))<.0001f,"Source navigation coordinate/winding changed");
                        if(edge.m_oppositeEdge!=uint.MaxValue){var opposite=mesh.m_edges[(int)edge.m_oppositeEdge];Check(opposite.m_oppositeEdge==index&&opposite.m_oppositeFace==f&&opposite.m_a==edge.m_b&&opposite.m_b==edge.m_a,"Native adjacency is not reciprocal");}
                    }
                    var node=graph.m_nodes[f];foreach(var edge in graph.m_edges.Skip(node.m_startEdgeIndex).Take(node.m_numEdges))Check(mesh.m_edges.Skip(face.m_startEdgeIndex).Take(3).Any(e=>e.m_oppositeFace==edge.m_target&&(e.m_flags&32)==0),"Graph fabricated adjacency or crossed blocked source face");
                }
                Bounds(mesh,tree);totalFaces+=mesh.m_faces.Count;totalGraphEdges+=graph.m_edges.Count;
            }
            Check(nva.FaceDataEntries.Count==totalFaces,"Native NVA face data length mismatch");
            Check(NVA.Read(nva.Write()).NavmeshInfoEntries.Count==nva.NavmeshInfoEntries.Count,"Native NVA roundtrip changed entries");
            reports.Add(new{map,target,placements=binder.Files.Count,faces=totalFaces,graphEdges=totalGraphEdges,report=export.Report});
            Console.WriteLine($"PASS: {map}: {binder.Files.Count} source placements, {totalFaces} native faces, {totalGraphEdges} source-adjacency graph edges");
        }
        Reject(()=>MapNavigation.Bake("DS2S","m10_02_00_00","m40_02_00_00",_=>throw new Exception("Unsupported profile read source"),offset));
        Reject(()=>MapNavigation.Bake("DS1R","bad","m40_02_00_00",Source,offset));
        foreach(var p in hashes)Check(Hash(File.ReadAllBytes(p.Key))==p.Value,"Original navigation asset was changed");
        string folder="out/map-editor/map-navigation-tests";Directory.CreateDirectory(folder);File.WriteAllText(Path.Combine(folder,"report.json"),JsonSerializer.Serialize(new{reports,preserved=hashes,limitations="Fixture/format proof only; native streaming and AI execution require gameplay QA."},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine("PASS: retail names/class contracts and BB tree-pointer offsets, source geometry/adjacency, complete BVH containment, NVA binding, blocked islands, invalid inputs and original asset hashes");
    }
    // Parse raw fixups independently: a managed DS3 write/read cycle previously
    // passed while native GCP dereferenced the untouched +0x10 pointer as NULL.
    static int NativeContract(byte[] b) {
        int I(int p)=>BitConverter.ToInt32(b,p);string S(int p)=>Encoding.ASCII.GetString(b,p,Array.IndexOf(b,(byte)0,p)-p);
        int h=64+BitConverter.ToInt16(b,62),names=I(h+20),d=I(h+148),local=I(h+152),global=I(h+156),virt=I(h+160),end=I(h+164);
        var classes=new Dictionary<int,string>();for(int p=d+virt;p<=d+end-12;p+=12)if(I(p)!=-1)classes.Add(I(p),S(names+I(p+8)));
        var locals=new Dictionary<int,int>();for(int p=d+local;p<=d+global-8;p+=8)if(I(p)!=-1)locals.Add(I(p),I(p+4));
        var globals=new Dictionary<int,(int Target,int Position)>();for(int p=d+global;p<=d+virt-12;p+=12)if(I(p)!=-1){Check(I(p+4)==2,"Native class pointer targets another section");globals.Add(I(p),(I(p+8),p));}
        int tree=classes.Single(p=>p.Value=="hkcdStaticAabbTree").Key,storage=classes.Single(p=>p.Value=="hkcdStaticTreeDefaultTreeStorage6").Key;
        Check(globals.TryGetValue(tree+16,out var pointer)&&pointer.Target==storage&&!globals.ContainsKey(tree+24),"BB tree storage must be fixed up at +0x10, not DS3 +0x18");
        Check(I(d+storage+8)>0&&locals.ContainsKey(storage),"Native BVH storage has no nodes");
        int variants=locals[0],count=I(d+8);for(int i=0;i<count;i++){int at=variants+i*24;string name=S(d+locals[at]),type=S(d+locals[at+8]);Check(name==type&&classes[globals[at+16].Target]==type,"Native root variant name/class lookup mismatch");}
        return pointer.Position;
    }
    static void Fixture() {
        var source=new NVM{Vertices=[new(0,0,0),new(0,0,1),new(1,0,0),new(1,0,1),new(5,0,0),new(5,0,1),new(6,0,0)],Triangles=[new(){VertexIndex1=0,VertexIndex2=1,VertexIndex3=2,EdgeIndex1=-1,EdgeIndex2=1,EdgeIndex3=-1},new(){VertexIndex1=2,VertexIndex2=1,VertexIndex3=3,EdgeIndex1=0,EdgeIndex2=-1,EdgeIndex3=-1},new(){VertexIndex1=4,VertexIndex2=5,VertexIndex3=6,EdgeIndex1=-1,EdgeIndex2=-1,EdgeIndex3=-1}],Entities=[]};
        var matrix=Matrix4x4.CreateRotationY(.3f)*Matrix4x4.CreateTranslation(3,4,5);var root=MapNavigation.Build(source,matrix).Root;var graph=(hkaiDirectedGraphExplicitCost)root.m_namedVariants[2].m_variant;Check(graph.m_nodes.Count==3&&graph.m_edges.Count==2&&graph.m_nodes[2].m_numEdges==0,"Disconnected island linked by synthetic graph");
        source.Triangles[1].Flags=NVM.TriangleFlags.BLOCK_GATE;root=MapNavigation.Build(source,matrix).Root;graph=(hkaiDirectedGraphExplicitCost)root.m_namedVariants[2].m_variant;Check(graph.m_edges.Count==0,"Unported source gate remains traversable");
        source.Triangles[1].EdgeIndex1=-1;Reject(()=>MapNavigation.Build(source,matrix));source.Triangles[1].EdgeIndex1=0;source.Triangles[0].VertexIndex1=99;Reject(()=>MapNavigation.Build(source,matrix));source.Triangles[0].VertexIndex1=0;source.Vertices[0]=new(float.NaN,0,0);Reject(()=>MapNavigation.Build(source,matrix));
    }
    static void Bounds(hkaiNavMesh mesh,hkcdStaticTreeDefaultTreeStorage6 tree) {
        var visited=new HashSet<int>();var leaves=new HashSet<int>();
        void Walk(int index,double[] min,double[] max) {
            Check(index>=0&&index<tree.m_nodes.Count&&visited.Add(index),"Native nav BVH cycle/index");var n=tree.m_nodes[index];var bytes=new[]{n.m_xyz_0,n.m_xyz_1,n.m_xyz_2};var lo=new double[3];var hi=new double[3];for(int a=0;a<3;a++){double unit=(max[a]-min[a])/226;lo[a]=min[a]+Math.Pow(bytes[a]>>4,2)*unit;hi[a]=max[a]-Math.Pow(bytes[a]&15,2)*unit;}
            int data=(n.m_hiData&127)*65536+n.m_loData;
            if((n.m_hiData&128)!=0){Walk(index+1,lo,hi);Walk(index+data*2,lo,hi);}else {
                Check(data<mesh.m_faces.Count&&leaves.Add(data),"Native nav BVH face coverage");var face=mesh.m_faces[data];foreach(var edge in mesh.m_edges.Skip(face.m_startEdgeIndex).Take(face.m_numEdges)){var v=mesh.m_vertices[edge.m_a];double[] p=[v.X,v.Y,v.Z];for(int a=0;a<3;a++)Check(p[a]>=lo[a]-.00005&&p[a]<=hi[a]+.00005,"Native nav BVH excludes serialized geometry");}
            }
        }
        var domain=tree.m_domain;Walk(0,[domain.m_min.X,domain.m_min.Y,domain.m_min.Z],[domain.m_max.X,domain.m_max.Y,domain.m_max.Z]);Check(visited.Count==tree.m_nodes.Count&&leaves.Count==mesh.m_faces.Count,"Native nav BVH omitted nodes/faces");
    }
}
