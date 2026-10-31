// SPDX-License-Identifier: GPL-2.0-or-later
using System.Numerics;
using System.Text.RegularExpressions;
using HKX2;
using HKX2.Builders;
using SoulsFormats;

// Native navigation is required even for player-only adjacent-map streaming:
// SprjWorldBlockNvm's unload loop indexes count-1 without handling count zero.
// See corridor-fixes/native-streaming for the shipped-binary evidence.
internal static class MapNavigation {
    internal record MeshReport(string SourcePart,string SourceModel,int ModelID,int NameID,int Vertices,int SourceFaces,int Faces,int OmittedDegenerateFaces,int BlockedFaces,int SourceEntities,int DisconnectedSourceEdges);
    internal record Report(string SourceMap,string TargetMap,IReadOnlyList<MeshReport> Meshes,IReadOnlyList<string> Limits);
    internal record Export(IReadOnlyDictionary<string,byte[]> Files,Report Report);
    const NVM.TriangleFlags Unsupported=NVM.TriangleFlags.INSIDE_WALL|NVM.TriangleFlags.BLOCK_GATE|NVM.TriangleFlags.CLOSED_DOOR|NVM.TriangleFlags.DOOR|NVM.TriangleFlags.HOLE|NVM.TriangleFlags.LADDER|NVM.TriangleFlags.FLOOR_TO_WALL|NVM.TriangleFlags.DEGENERATE|NVM.TriangleFlags.WALL|NVM.TriangleFlags.BLOCK|NVM.TriangleFlags.GATE|NVM.TriangleFlags.DISABLE|NVM.TriangleFlags.EVENT;

    internal static Export Bake(string profile,string map,string target,Func<string,byte[]?> readSource,Vector3 offset) {
        if(profile!="DS1R")throw new NotSupportedException("Source navigation conversion currently supports DS1 Remastered only");
        if(!Regex.IsMatch(map,@"^m\d{2}_\d{2}_\d{2}_\d{2}$")||!Regex.IsMatch(target,@"^m\d{2}_\d{2}_\d{2}_\d{2}$")||!Finite(offset))throw new InvalidDataException("Invalid navigation map/frame");
        var msb=MSB1.Read(readSource($"map/MapStudio/{map}.msb")??readSource($"map/MapStudio/{map}.msb.dcx")??throw new FileNotFoundException("Source navigation MSB unavailable"));
        if(msb.Parts.Navmeshes.Count is <1 or >512)throw new InvalidDataException("Navigation requires 1..512 source placements per map");
        var source=BND3.Read(readSource($"map/{map}/{map}.nvmbnd.dcx")??throw new FileNotFoundException("Source NVM binder unavailable"));
        var members=source.Files.ToDictionary(f=>Path.GetFileName(f.Name.Replace('\\','/')),StringComparer.OrdinalIgnoreCase);
        var binder=new BND4{Version="07D7R6"};var nva=new NVA{Version=NVA.NVAVersion.DarkSouls3,SectionCount=9,NavmeshInfoEntries=new(3)};var reports=new List<MeshReport>();
        int code=int.Parse(target.Substring(1,2))*100+int.Parse(target.Substring(4,2));
        foreach(var part in msb.Parts.Navmeshes) {
            if(part.ModelName==null||!Regex.IsMatch(part.ModelName,@"^n\d{4}B\d$"))throw new InvalidDataException("Unsupported source navigation model: "+part.ModelName);
            string file=part.ModelName+"A"+map.Substring(1,2)+".nvm";
            if(!members.TryGetValue(file,out var member))throw new FileNotFoundException("Source navmesh unavailable: "+file);
            var raw=DCX.Is(member.Bytes)?DCX.Decompress(member.Bytes).ToArray():member.Bytes.ToArray();
            if(raw.Length<128||BitConverter.ToInt32(raw)!=1||BitConverter.ToInt32(raw,4) is <1 or >1000000||BitConverter.ToInt32(raw,12) is <1 or >100000)throw new InvalidDataException("Unsupported source NVM counts/header");
            var nvm=NVM.Read(raw);var rot=part.Rotation*(MathF.PI/180);
            if(!Finite(part.Position)||!Finite(rot)||!Finite(part.Scale)||part.Scale.X<=0||part.Scale.Y<=0||part.Scale.Z<=0)throw new InvalidDataException("Invalid navigation placement transform");
            var transform=Matrix4x4.CreateScale(part.Scale)*Matrix4x4.CreateRotationX(rot.X)*Matrix4x4.CreateRotationZ(rot.Z)*Matrix4x4.CreateRotationY(rot.Y)*Matrix4x4.CreateTranslation(part.Position+offset);
            int model=reports.Count,name=checked(code*100000+model*10);
            var (root,faces,blocked,omitted,disconnected)=Build(nvm,transform);
            var mesh=(hkaiNavMesh)root.m_namedVariants[0].m_variant;
            byte[] bytes;using(var stream=new MemoryStream()){new PackFileSerializer().Serialize(root,new BinaryWriterEx(false,stream));bytes=stream.ToArray();}
            // BB differs from the DS3 serializer in both the padding byte and
            // hkcdStaticAabbTree::m_treePtr offset; signatures alone miss this.
            bytes[18]=1;
            MoveTreePointer(bytes,24,16);
            var roundtrip=ReadMesh(bytes);if(roundtrip.m_faces.Count!=faces||roundtrip.m_edges.Count!=faces*3||roundtrip.m_vertices.Count!=mesh.m_vertices.Count)throw new InvalidDataException("Native navigation serialization lost geometry");
            string memberName=$@"N:\SPRJ\data\INTERROOT_ps4\map\{target}\navimesh\bind6\n{target[1..]}_{model:D6}.hkx";
            binder.Files.Add(new(Binder.FileFlags.Flag1,10000+model,memberName,bytes));
            nva.NavmeshInfoEntries.Add(new(){NameID=name,ModelID=model,Position=Vector3.Zero,Rotation=Vector3.Zero,Scale=Vector3.One,FaceDataIndex=nva.FaceDataEntries.Count,FaceCount=faces});
            for(int i=0;i<faces;i++)nva.FaceDataEntries.Add(new());
            reports.Add(new(part.Name,part.ModelName,model,name,mesh.m_vertices.Count,nvm.Triangles.Count,faces,omitted,blocked,nvm.Entities.Count,disconnected));
        }
        var files=new Dictionary<string,byte[]>(StringComparer.Ordinal){[$"map/{target}/{target}.nva.dcx"]=nva.Write(DCX.Type.DCX_DFLT_10000_44_9),[$"map/{target}/{target}.nvmhktbnd.dcx"]=binder.Write(DCX.Type.DCX_DFLT_10000_44_9)};
        return new(files,new(map,target,reports,["Within-mesh source adjacency only; source MCG/MCP cross-mesh and cross-map navigation connections are not converted.","Source gates, doors, ladders, event-controlled faces and disabled/wall/hole faces are conservatively blocked; their state machines and NVM entities are not ported.","Degenerate source triangles are omitted; source portals whose authored endpoints differ are disconnected and counted as directed edges. Native graph nodes correspond to individual source faces; no paths are fabricated between disconnected islands.","Source NvmGroups, dynamic obstacles, user edges and navmesh erosion/agent-size compatibility are not converted; native erosionRadius remains zero for already-authored source geometry."]));
    }

    internal static hkaiNavMesh ReadMesh(byte[] bytes) {
        return ReadRoot(bytes).m_namedVariants.Select(v=>v.m_variant).OfType<hkaiNavMesh>().Single();
    }

    internal static hkRootLevelContainer ReadRoot(byte[] bytes) {
        if(bytes.Length<80||BitConverter.ToUInt32(bytes)!=0x57e0e057||bytes[16]!=8||bytes[17]!=1||bytes[18]!=1||System.Text.Encoding.ASCII.GetString(bytes,40,14)!="hk_2014.1.0-r1")throw new InvalidDataException("Expected native 64-bit little-endian Havok 2014 navigation");
        // Adapt a private copy for the external library's DS3 class reader.
        // Retail BB and our emitted bytes retain the actual native +0x10 slot.
        var copy=bytes.ToArray();MoveTreePointer(copy,16,24);
        return (hkRootLevelContainer)new PackFileDeserializer().Deserialize(new BinaryReaderEx(false,copy));
    }

    static void MoveTreePointer(byte[] bytes,int from,int to) {
        int U(int at){if(at<0||at>bytes.Length-4)throw new InvalidDataException("Truncated native navigation packfile");return BitConverter.ToInt32(bytes,at);}
        string Text(int at){if(at<0||at>=bytes.Length)throw new InvalidDataException("Invalid navigation class name");int end=Array.IndexOf(bytes,(byte)0,at);if(end<0||end-at>128)throw new InvalidDataException("Invalid navigation class name");return System.Text.Encoding.ASCII.GetString(bytes,at,end-at);}
        if(bytes.Length<80||U(0)!=0x57e0e057||U(4)!=0x10c0c010||U(12)!=11||U(20)!=3||U(24)!=2)throw new InvalidDataException("Unsupported native navigation packfile sections");
        int sections=checked(64+BitConverter.ToInt16(bytes,62)),header=checked(sections+128);
        if(Text(sections)!="__classnames__"||Text(header)!="__data__")throw new InvalidDataException("Unsupported navigation section order");
        int names=U(sections+20),data=U(header+20),local=U(header+24),global=U(header+28),virt=U(header+32),end=U(header+36);
        if(names<0||data<0||local<0||global<local||virt<global||end<virt||data>bytes.Length-end)throw new InvalidDataException("Invalid navigation fixup ranges");
        var classes=new Dictionary<int,string>();
        for(int p=data+virt;p<=data+end-12;p+=12)if(U(p)!=-1){if(U(p+4)!=0)throw new InvalidDataException("Invalid navigation class section");classes.Add(U(p),Text(checked(names+U(p+8))));}
        int tree=classes.Single(p=>p.Value=="hkcdStaticAabbTree").Key,storage=classes.Single(p=>p.Value=="hkcdStaticTreeDefaultTreeStorage6").Key,found=-1;
        for(int p=data+global;p<=data+virt-12;p+=12) {
            int at=U(p);if(at==-1)continue;
            if(at==tree+to)throw new InvalidDataException("Unexpected native navigation tree pointer layout");
            if(at==tree+from){if(found!=-1||U(p+4)!=2||U(p+8)!=storage)throw new InvalidDataException("Invalid native navigation tree pointer");found=p;}
        }
        if(found<0||tree<0||tree>local-32||storage<0||storage>local-48||BitConverter.ToUInt64(bytes,data+tree+from)!=0||BitConverter.ToUInt64(bytes,data+tree+to)!=0)throw new InvalidDataException("Missing native navigation tree storage");
        // Native StaticTree::GCP thunk 0x5b1830 reads [tree+0x10]. The
        // external DS3 class inserts an extra UInt64 before its +0x18 pointer.
        // Move only the fixup source; objects, storage and all geometry stay put.
        BitConverter.TryWriteBytes(bytes.AsSpan(found,4),tree+to);
    }

    internal static (hkRootLevelContainer Root,int Faces,int Blocked,int Omitted,int DisconnectedSourceEdges) Build(NVM source,Matrix4x4 transform) {
        if(source.BigEndian||source.Vertices.Count is <1 or >1000000||source.Triangles.Count is <1 or >100000)throw new InvalidDataException("Unsupported NVM mesh");
        var points=source.Vertices.Select(v=>Vector3.Transform(v,transform)).ToArray();if(points.Any(v=>!Finite(v)))throw new InvalidDataException("Non-finite NVM coordinates");
        int[] Indices(NVM.Triangle t)=>[t.VertexIndex1,t.VertexIndex2,t.VertexIndex3];
        int[] Neighbours(NVM.Triangle t)=>[t.EdgeIndex1,t.EdgeIndex2,t.EdgeIndex3];
        var retained=new List<int>();var faceMap=Enumerable.Repeat(-1,source.Triangles.Count).ToArray();
        for(int i=0;i<source.Triangles.Count;i++) {
            var ix=Indices(source.Triangles[i]);if(ix.Any(v=>v<0||v>=points.Length)||Neighbours(source.Triangles[i]).Any(v=>v< -1||v>=source.Triangles.Count))throw new InvalidDataException("NVM vertex/adjacency index out of range");
            if(Vector3.Cross(points[ix[1]]-points[ix[0]],points[ix[2]]-points[ix[0]]).LengthSquared()<1e-12f)continue;
            faceMap[i]=retained.Count;retained.Add(i);
        }
        if(retained.Count is <1 or >30000)throw new InvalidDataException("Native navigation requires 1..30000 nondegenerate faces per placement");
        var used=retained.SelectMany(i=>Indices(source.Triangles[i])).Distinct().Order().ToArray();var remap=used.Select((v,i)=>(v,i)).ToDictionary(p=>p.v,p=>p.i);
        var mesh=new hkaiNavMesh{m_faces=[],m_edges=[],m_vertices=used.Select(i=>new Vector4(points[i],1)).ToList(),m_streamingSets=[],m_faceData=[],m_edgeData=[],m_faceDataStriding=1,m_edgeDataStriding=1,m_flags=0,m_erosionRadius=0,m_aabb=new()};
        var graph=new hkaiDirectedGraphExplicitCost{m_positions=[],m_nodes=[],m_edges=[],m_nodeData=[],m_edgeData=[],m_streamingSets=[],m_nodeDataStriding=0,m_edgeDataStriding=0};
        var centroids=retained.Select(i=>{var ix=Indices(source.Triangles[i]);return (points[ix[0]]+points[ix[1]]+points[ix[2]])/3;}).ToArray();
        bool Blocked(int i)=>(source.Triangles[i].Flags&Unsupported)!=0;int disconnected=0;
        for(int face=0;face<retained.Count;face++) {
            int original=retained[face];var t=source.Triangles[original];var ix=Indices(t);var neighbours=Neighbours(t);var node=new hkaiDirectedGraphExplicitCostNode{m_startEdgeIndex=graph.m_edges.Count};
            mesh.m_faces.Add(new(){m_startEdgeIndex=face*3,m_startUserEdgeIndex=-1,m_numEdges=3,m_numUserEdges=0,m_clusterIndex=checked((short)face),m_padding=0xcdcd});mesh.m_faceData.Add(0);
            for(int edge=0;edge<3;edge++) {
                int a=ix[edge],b=ix[(edge+1)%3],other=neighbours[edge];
                var e=new hkaiNavMeshEdge{m_a=remap[a],m_b=remap[b],m_oppositeEdge=uint.MaxValue,m_oppositeFace=uint.MaxValue,m_flags=4};
                bool blocked=Blocked(original);
                if(other>=0&&faceMap[other]>=0) {
                    var oi=Indices(source.Triangles[other]);var on=Neighbours(source.Triangles[other]);int reverse=Array.FindIndex(on,v=>v==original);
                    if(reverse<0)throw new InvalidDataException("Non-reciprocal source NVM adjacency");
                    blocked|=Blocked(other);
                    if(oi[reverse]==b&&oi[(reverse+1)%3]==a) {
                        e.m_oppositeFace=(uint)faceMap[other];e.m_oppositeEdge=(uint)(faceMap[other]*3+reverse);
                        if(!blocked){var cost=(Half)Vector3.Distance(centroids[face],centroids[faceMap[other]]);if(!Half.IsFinite(cost))throw new InvalidDataException("Navigation graph cost out of range");graph.m_edges.Add(new(){m_target=(uint)faceMap[other],m_cost=BitConverter.HalfToInt16Bits(cost),m_flags=0});node.m_numEdges++;}
                    } else disconnected++;
                }
                if(blocked)e.m_flags|=32;mesh.m_edges.Add(e);mesh.m_edgeData.Add(0);
            }
            graph.m_positions.Add(new Vector4(centroids[face],1));graph.m_nodes.Add(node);
        }
        var leaves=retained.Select((i,face)=>{var ix=Indices(source.Triangles[i]);return new BVNode{IsLeaf=true,Primitive=(uint)face,PrimitiveCount=1,Min=ix.Select(v=>points[v]).Aggregate(Vector3.Min)-new Vector3(.001f),Max=ix.Select(v=>points[v]).Aggregate(Vector3.Max)+new Vector3(.001f)};}).ToList();
        BVNode Tree(List<BVNode> nodes){if(nodes.Count==1)return nodes[0];var lo=nodes.Select(n=>n.Min).Aggregate(Vector3.Min);var hi=nodes.Select(n=>n.Max).Aggregate(Vector3.Max);var d=hi-lo;int axis=d.X>d.Y?(d.X>d.Z?0:2):(d.Y>d.Z?1:2);float Key(BVNode n)=>axis==0?n.Min.X+n.Max.X:axis==1?n.Min.Y+n.Max.Y:n.Min.Z+n.Max.Z;nodes=nodes.OrderBy(Key).ThenBy(n=>n.Primitive).ToList();int mid=nodes.Count/2;return new(){Min=lo,Max=hi,Left=Tree(nodes.GetRange(0,mid)),Right=Tree(nodes.GetRange(mid,nodes.Count-mid))};}
        var tree=Tree(leaves);mesh.m_aabb.m_min=new(tree.Min,1);mesh.m_aabb.m_max=new(tree.Max,1);
        var aabb=new hkcdStaticAabbTree{m_treePtr=new(){m_nodes=tree.BuildAxis6Tree(),m_domain=new(){m_min=new(tree.Min,1),m_max=new(tree.Max,1)}}};
        var root=new hkRootLevelContainer{m_namedVariants=[]};foreach(var (name,obj) in new (string,hkReferencedObject)[]{("hkaiNavMesh",mesh),("hkcdStaticAabbTree",aabb),("hkaiDirectedGraphExplicitCost",graph)})root.m_namedVariants.Add(new(){m_name=name,m_className=name,m_variant=obj});
        return (root,retained.Count,retained.Count(Blocked),source.Triangles.Count-retained.Count,disconnected);
    }
    static bool Finite(Vector3 v)=>float.IsFinite(v.X)&&float.IsFinite(v.Y)&&float.IsFinite(v.Z)&&Math.Abs(v.X)<1000000&&Math.Abs(v.Y)<1000000&&Math.Abs(v.Z)<1000000;
}
