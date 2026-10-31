// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;

static class SkinEquipmentTests {
    static void Check(bool ok,string reason){if(!ok)throw new Exception(reason);}
    static string Leaf(string name)=>name.Replace('\\','/').Split('/').Last();
    static string Key(string name)=>new(name.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
    static bool Attachment(string name)=>System.Text.RegularExpressions.Regex.IsMatch(Key(name),@"\A(?:rweapon\d*|lweapon\d*|lshield|sword|bpw00)\z");
    static void Reject(Action action,string reason){try{action();}catch(InvalidDataException){return;}throw new Exception(reason);}
    static int MountedEquipment(string model,byte[] sourceModel,byte[] nativeModel,List<TPF.Texture> textures,IReadOnlySet<int> masks,SourceEquipmentMounts.Result mounts) {
        var source=FLVER2.Read(sourceModel);var native=FLVER2.Read(nativeModel);var sw=SkinRetarget.World(source.Nodes);var nw=SkinRetarget.World(native.Nodes);
        var baseline=SkinRetarget.Bake(sourceModel,nativeModel,textures,masks:masks,sourceProfile:"DS1R",fitSegments:true,preserveBoneLengths:true);
        var mounted=SkinRetarget.Bake(sourceModel,nativeModel,textures,masks:masks,sourceProfile:"DS1R",fitSegments:true,preserveBoneLengths:true,equipmentMounts:mounts.Frames);
        var before=FLVER2.Read(baseline.Model.Write());var after=FLVER2.Read(mounted.Model.Write());var selected=source.Meshes.Where(m=>{var mask=System.Text.RegularExpressions.Regex.Match(source.Materials[m.MaterialIndex].Name,@"#(\d+)#");return !mask.Success||masks.Contains(int.Parse(mask.Groups[1].Value));}).ToArray();int checkedVertices=0;
        for(int mesh=0;mesh<selected.Length;mesh++) {
            for(int vertex=0;vertex<selected[mesh].Vertices.Count;vertex++) {
                var old=selected[mesh].Vertices[vertex];var expected=before.Meshes[mesh].Vertices[vertex].Position;bool affected=false;float sum=Enumerable.Range(0,4).Sum(j=>old.BoneWeights[j]);
                for(int slot=0;slot<4;slot++) {
                    if(old.BoneWeights[slot]==0)continue;int bone=SkinRetarget.Bone(selected[mesh],old.BoneIndices[slot]);if(!mounts.Frames.TryGetValue(source.Nodes[bone].Name,out var local))continue;affected=true;
                    int parent=source.Nodes[bone].ParentIndex,target=native.Nodes.FindIndex(n=>n.Name==source.Nodes[parent].Name);Check(target>=0,"Native equipment parent missing");Check(Matrix4x4.Invert(sw[bone],out var inverse),"Singular source equipment mount bind");Check(Matrix4x4.Invert(sw[parent],out var parentInverse),"Singular source equipment parent bind");Check(Matrix4x4.Invert(nw[target],out var nativeInverse),"Singular native equipment mount bind");
                    var previous=parentInverse*nw[target];if(Key(source.Nodes[parent].Name) is "lforearm" or "rforearm") {string end=source.Nodes[parent].Name.StartsWith('L')?"L Hand":"R Hand";int a=source.Nodes.FindIndex(n=>n.Name==end),b=native.Nodes.FindIndex(n=>n.Name==end);previous=SkinRetarget.FitSegment(previous,sw[parent].Translation,sw[a].Translation,nw[target].Translation,nw[b].Translation);}
                    expected+=(Vector3.Transform(old.Position,inverse*local*nw[target])-Vector3.Transform(old.Position,previous))*(old.BoneWeights[slot]/sum);
                    if(old.BoneWeights[slot]>.9999f) {
                // Compare in parent space against the ORIGINAL source gear bind
                // plus its authored animation-local mount, independently of the
                // retargeter's parent bind/segment fitting implementation.
                        var actual=after.Meshes[mesh].Vertices[vertex];Check(actual.BoneIndices[0]==target&&actual.BoneWeights[0]==1,"Authored equipment lost its anatomical parent");Check(Vector3.Distance(Vector3.Transform(actual.Position,nativeInverse),Vector3.Transform(old.Position,inverse*local))<.00001f,"Source authored local equipment grip was not preserved");checkedVertices++;
                    }
                }
                if(!affected)continue;Check(Vector3.Distance(after.Meshes[mesh].Vertices[vertex].Position,expected)<.00001f,"Mixed equipment/body skin did not retain its authored weighted mount");Check(after.Meshes[mesh].Vertices[vertex].BoneIndices.Equals(before.Meshes[mesh].Vertices[vertex].BoneIndices)&&after.Meshes[mesh].Vertices[vertex].BoneWeights.Equals(before.Meshes[mesh].Vertices[vertex].BoneWeights),"Equipment mount changed anatomical skin weights");after.Meshes[mesh].Vertices[vertex]=before.Meshes[mesh].Vertices[vertex];
            }
            after.Meshes[mesh].BoundingBox=before.Meshes[mesh].BoundingBox;
        }
        after.Header.BoundingBoxMin=before.Header.BoundingBoxMin;after.Header.BoundingBoxMax=before.Header.BoundingBoxMax;Check(after.Write().SequenceEqual(before.Write()),"Authored equipment mount changed body/cloth/masks/UVs/colors/materials/topology or unrelated native fields");Check(mounted.Textures.Write().SequenceEqual(baseline.Textures.Write()),"Source equipment mount changed texture payloads");
        Check(mounts.Report.Mounts.All(m=>m.MaximumIdleVariation<.002f),"Selected source idle attachment is not constant within codec precision");
        if(model=="c2560") {var shield=mounts.Report.Mounts.Single(m=>Key(m.Bone)=="lshield");Check(shield.TranslationFromReference is >.25f and <.27f&&shield.RotationFromReference is >.55f and <.60f,"Source Balder shield animation offset was omitted");}
        Reject(()=>SkinRetarget.Bake(sourceModel,nativeModel,textures,masks:masks,sourceProfile:"BB",fitSegments:true,preserveBoneLengths:true,equipmentMounts:mounts.Frames),"Native/default path accepted source equipment mounts");
        return checkedVertices;
    }
    static void StaticCape(SourceClothPose.Result settled,byte[] nativeModel,List<TPF.Texture> textures,IReadOnlySet<int> masks) {
        Check(settled.Report.RigidCapeAnchor=="Spine1"&&settled.Report.CapeFixedAnchors.SequenceEqual(new[]{"L UpperArm","R UpperArm","Spine1"}),"Static cape anchor was not derived from authored fixed capsule anatomy");
        var source=FLVER2.Read(settled.Model);var capeIndices=settled.Anchors.Keys.Order().ToArray();Check(capeIndices.SequenceEqual(new[]{3,4})&&settled.Anchors.Values.All(n=>n=="Spine1"),"Static cape mesh scope changed");
        var ordinary=SkinRetarget.Bake(settled.Model,nativeModel,textures,masks:masks,sourceProfile:"DS1R",fitSegments:true,preserveBoneLengths:true);
        var rigid=SkinRetarget.Bake(settled.Model,nativeModel,textures,masks:masks,sourceProfile:"DS1R",fitSegments:true,preserveBoneLengths:true,rigidMeshAnchors:settled.Anchors);
        var before=FLVER2.Read(ordinary.Model.Write());var after=FLVER2.Read(rigid.Model.Write());var native=FLVER2.Read(nativeModel);int anchor=after.Nodes.FindIndex(n=>n.Name=="Spine1");Check(anchor>=0,"Native cape torso anchor missing");
        var selected=Enumerable.Range(0,source.Meshes.Count).Where(i=>{var mask=System.Text.RegularExpressions.Regex.Match(source.Materials[source.Meshes[i].MaterialIndex].Name,@"#(\d+)#");return !mask.Success||masks.Contains(int.Parse(mask.Groups[1].Value));}).ToArray();var capeOutput=Enumerable.Range(0,selected.Length).Where(i=>settled.Anchors.ContainsKey(selected[i])).ToArray();Check(capeOutput.Sum(i=>after.Meshes[i].Vertices.Count)==337,"Static cape lost authored vertices");
        var bind=SkinRetarget.World(after.Nodes);var posed=FLVER2.Read(after.Write());foreach(var node in posed.Nodes.Where(n=>n.Name is "L UpperArm" or "R UpperArm" or "L Forearm" or "R Forearm"))node.Rotation+=new Vector3(.7f,-.45f,1.1f);var armWorld=SkinRetarget.World(posed.Nodes);
        Vector3 Position(FLVER2.Mesh mesh,FLVER.Vertex v,IReadOnlyList<Matrix4x4> world){var position=Vector3.Zero;for(int j=0;j<4;j++)if(v.BoneWeights[j]>0){int node=SkinRetarget.Bone(mesh,v.BoneIndices[j]);Check(Matrix4x4.Invert(bind[node],out var inverse),"Singular cape skin bind");position+=Vector3.Transform(v.Position,inverse*world[node])*v.BoneWeights[j];}return position;}
        float oldArmPull=0;
        foreach(int i in capeOutput) {
            var mesh=after.Meshes[i];var old=before.Meshes[i];Check(after.Materials[mesh.MaterialIndex].MTD.EndsWith("C[ARSN]_e.mtd",StringComparison.Ordinal),"Static cape lost its authored cutout alpha material");
            for(int v=0;v<mesh.Vertices.Count;v++) {var vertex=mesh.Vertices[v];Check(vertex.BoneIndices[0]==anchor&&vertex.BoneWeights[0]==1&&Enumerable.Range(1,3).All(j=>vertex.BoneWeights[j]==0),"A static cape vertex retained an independently moving arm frame");Check(Vector3.Distance(Position(mesh,vertex,armWorld),vertex.Position)<.00001f,"Native arm animation deformed a rigid static cape");oldArmPull=Math.Max(oldArmPull,Vector3.Distance(Position(old,old.Vertices[v],armWorld),old.Vertices[v].Position));}
            // A changing torso transform must move the whole cape as one rigid
            // surface. Check authored triangle edge lengths independently.
            posed.Nodes[anchor].Rotation+=new Vector3(.2f,-.15f,.35f);var torsoWorld=SkinRetarget.World(posed.Nodes);var positions=mesh.Vertices.Select(v=>Position(mesh,v,torsoWorld)).ToArray();
            foreach(var face in mesh.FaceSets)for(int t=0;t<face.Indices.Count;t+=3)for(int e=0;e<3;e++){int a=face.Indices[t+e],b=face.Indices[t+(e+1)%3];Check(Math.Abs(Vector3.Distance(positions[a],positions[b])-Vector3.Distance(mesh.Vertices[a].Position,mesh.Vertices[b].Position))<.00001f,"Native torso animation stretched the static cape");}
            posed.Nodes[anchor].Rotation-=new Vector3(.2f,-.15f,.35f);
            after.Meshes[i]=before.Meshes[i];
        }
        Check(oldArmPull>.25f,"Arm-motion regression does not expose the former cape wing deformation");after.Header.BoundingBoxMin=before.Header.BoundingBoxMin;after.Header.BoundingBoxMax=before.Header.BoundingBoxMax;Check(after.Write().SequenceEqual(before.Write()),"Static cape anchors changed body, equipment, masks, textures, source topology or unrelated native fields");
        Reject(()=>SkinRetarget.Bake(settled.Model,nativeModel,textures,masks:masks,sourceProfile:"BB",fitSegments:true,preserveBoneLengths:true,rigidMeshAnchors:settled.Anchors),"Native/default profile accepted source-only cape policy");
        Reject(()=>SkinRetarget.Bake(settled.Model,nativeModel,textures,masks:masks,sourceProfile:"DS1R",fitSegments:true,rigidMeshAnchors:settled.Anchors),"Skeleton/friendly path accepted ordinary-enemy cape policy");
        Console.WriteLine($"PASS static cape:337 serialized vertices, authored Spine1 ancestor, independent arm pull {oldArmPull:F3}m removed; torso shape/body/equipment preserved");
    }
    public static void Main(string[] args) {
        string source=args[0],native=args[1],defs=args[2],output=args[3];var hashes=new Dictionary<string,byte[]>();
        byte[] Read(string path){var bytes=File.ReadAllBytes(path);hashes.TryAdd(path,SHA256.HashData(bytes));return bytes;}
        var nativeBnd=BND4.Read(Read(Path.Combine(native,"chr/c2610.chrbnd.dcx")));var nativeModel=nativeBnd.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();
        var animation=BND4.Read(Read(Path.Combine(native,"chr/c2610.anibnd.dcx")));var skeleton=animation.Files.Single(f=>f.ID==1000000).Bytes.ToArray();
        var sourceBnd=BND3.Read(Read(Path.Combine(source,"param/GameParam/GameParam.parambnd.dcx")));var npcs=PARAM.Read(sourceBnd.Files.Single(f=>Leaf(f.Name)=="NpcParam.param").Bytes);npcs.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(defs,"NpcParam.xml")));
        var variants=new Dictionary<string,int[]>{{"c2500",[250000,250001]},{"c2540",[254000,254001,254002]},{"c2550",[255000,255001,255002]},{"c2560",[256000,256002]}};
        var reports=new List<object>();int checkedVertices=0,checkedWeapons=0,mountedVertices=0;
        foreach(var (model,rows) in variants) {
            var binder=BND3.Read(Read(Path.Combine(source,"chr",model+".chrbnd.dcx")));var sourceBytes=binder.Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes.ToArray();var src=FLVER2.Read(sourceBytes);var sw=SkinRetarget.World(src.Nodes);
            var textures=new List<TPF.Texture>();foreach(var file in binder.Files)if(TPF.Is(file.Bytes.ToArray()))textures.AddRange(TPF.Read(file.Bytes).Textures);
            foreach(var header in binder.Files.Where(f=>Leaf(f.Name).EndsWith(".chrtpfbhd"))) {using var split=new BXF3Reader(header.Bytes,Read(Path.Combine(source,"chr",model+".chrtpfbdt")));foreach(var file in split.Files)textures.AddRange(TPF.Read(split.ReadFile(file)).Textures);}
            foreach(string owner in src.Materials.SelectMany(m=>m.Textures).Select(t=>System.Text.RegularExpressions.Regex.Match(t.Path.Replace('\\','/'),@"(?:^|/)chr/(c\d{4})/",System.Text.RegularExpressions.RegexOptions.IgnoreCase)).Where(m=>m.Success).Select(m=>m.Groups[1].Value).Where(o=>o!=model).Distinct()) {
                var shared=BND3.Read(Read(Path.Combine(source,"chr",owner+".chrbnd.dcx")));
                foreach(var file in shared.Files)if(TPF.Is(file.Bytes.ToArray()))textures.AddRange(TPF.Read(file.Bytes).Textures);
                foreach(var header in shared.Files.Where(f=>Leaf(f.Name).EndsWith(".chrtpfbhd"))) {using var split=new BXF3Reader(header.Bytes,Read(Path.Combine(source,"chr",owner+".chrtpfbdt")));foreach(var file in split.Files)textures.AddRange(TPF.Read(split.ReadFile(file)).Textures);}
            }
            var plan=EnemyProportions.Create(sourceBytes,nativeModel,skeleton);var nativeRig=FLVER2.Read(plan.Model);var tw=SkinRetarget.World(nativeRig.Nodes);
            SourceClothPose.Result? settled=model=="c2560"?SourceClothPose.Bake(sourceBytes,binder.Files.Single(f=>Leaf(f.Name)=="c2560_c.hkx").Bytes.ToArray()):null;
            foreach(int rowID in rows) {
                var row=npcs.Rows.Single(r=>r.ID==rowID);var masks=Enumerable.Range(0,16).Where(i=>Convert.ToInt32(row.Cells.Single(c=>c.Def.InternalName=="modelDispMask"+i).Value)!=0).ToHashSet();
                var mounts=SourceEquipmentMounts.Load(p=>Read(Path.Combine(source,p)),"DS1R",model,sourceBytes,masks)??throw new Exception("Source equipment mount adapter missing");mountedVertices+=MountedEquipment(model,sourceBytes,plan.Model,textures,masks,mounts);
                if(settled!=null)StaticCape(settled,plan.Model,textures,masks);
                bool Selected(FLVER2.Mesh m){var mask=System.Text.RegularExpressions.Regex.Match(src.Materials[m.MaterialIndex].Name,@"#(\d+)#");return !mask.Success||masks.Contains(int.Parse(mask.Groups[1].Value));}
                var selected=src.Meshes.Where(Selected).ToArray();
                var bake=SkinRetarget.Bake(sourceBytes,plan.Model,textures,masks:masks,sourceProfile:"DS1R",fitSegments:true,preserveBoneLengths:true);
                var result=FLVER2.Read(bake.Model.Write());Check(result.Meshes.Count==selected.Length,"Source loadout mesh count changed");
                var socketChanged=FLVER2.Read(plan.Model);foreach(var node in socketChanged.Nodes.Where(n=>Key(n.Name) is "weapon00" or "weapon01")){node.Translation+=new Vector3(3,-2,1);node.Rotation+=new Vector3(.7f,-.3f,1.2f);}
                var independent=SkinRetarget.Bake(sourceBytes,socketChanged.Write(),textures,masks:masks,sourceProfile:"DS1R",fitSegments:true,preserveBoneLengths:true);
                for(int meshIndex=0;meshIndex<selected.Length;meshIndex++) {
                    var mesh=selected[meshIndex];var imported=result.Meshes[meshIndex];
                    bool edge=src.Materials[mesh.MaterialIndex].MTD.EndsWith("_Edge.mtd",StringComparison.OrdinalIgnoreCase);
                    Check(result.Materials[imported.MaterialIndex].MTD.EndsWith(edge?"C[ARSN]_e.mtd":"C[ARSN].mtd",StringComparison.Ordinal),"Authored cutout armour/cape shader was lost");
                    Check(mesh.Vertices.Count==imported.Vertices.Count&&mesh.FaceSets.Count==imported.FaceSets.Count,"Source mesh/LOD population changed");
                    for(int f=0;f<mesh.FaceSets.Count;f++)Check(mesh.FaceSets[f].Triangulate(mesh.Vertices.Count<65535).SequenceEqual(imported.FaceSets[f].Indices),"Equipment faces/LODs changed");
                    for(int i=0;i<mesh.Vertices.Count;i++) {
                        var v=mesh.Vertices[i];var actual=imported.Vertices[i];Check(actual.Position==independent.Model.Meshes[meshIndex].Vertices[i].Position,"Donor torch socket changed imported geometry");
                        Check(v.UVs.SequenceEqual(actual.UVs.Take(v.UVs.Count))&&v.Colors.SequenceEqual(actual.Colors.Take(v.Colors.Count)),"Equipment UV/color changed");
                        int sourceBone=SkinRetarget.Bone(mesh,v.BoneIndices[0]);if(v.BoneWeights[0]<.9999f||!Attachment(src.Nodes[sourceBone].Name))continue;
                        int parent=src.Nodes[sourceBone].ParentIndex;string anchorName=src.Nodes[parent].Name;int target=nativeRig.Nodes.FindIndex(n=>n.Name==anchorName);Check(target>=0,"Missing anatomical equipment anchor");
                        Check(actual.BoneIndices[0]==target&&actual.BoneWeights[0]>.9999f,"Equipment changed its authored anatomical anchor: "+model+"/"+src.Nodes[sourceBone].Name);
                        Check(Matrix4x4.Invert(sw[parent],out var inverse),"Source anchor singular");var delta=inverse*tw[target];
                        if(Key(anchorName) is "lforearm" or "rforearm") {
                            string end=anchorName.StartsWith('L')?"L Hand":"R Hand";int sourceEnd=src.Nodes.FindIndex(n=>n.Name==end),targetEnd=nativeRig.Nodes.FindIndex(n=>n.Name==end);
                            delta=SkinRetarget.FitSegment(delta,sw[parent].Translation,sw[sourceEnd].Translation,tw[target].Translation,tw[targetEnd].Translation);
                        }
                        Check(Vector3.Distance(actual.Position,Vector3.Transform(v.Position,delta))<.00001f,"Equipment offset/grip frame changed");checkedVertices++;
                    }
                }
                foreach(var b in bake.Bones.Where(b=>Attachment(b.Source))){string target=b.Target;Check(target.Contains("Hand")||target.Contains("Forearm"),"Equipment still follows donor torch socket");checkedWeapons++;}
                reports.Add(new{model,rowID,masks=masks.Order().ToArray(),selectedMeshes=selected.Length,attachments=bake.Bones.Where(b=>Attachment(b.Source)).ToArray(),mounts=mounts.Report});
            }
        }
        foreach(var (path,hash) in hashes)Check(SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(hash),"Original game asset changed: "+path);
        Check(SourceEquipmentMounts.Load(_=>throw new Exception("Unexpected source read"),"BB","c2560",[],new HashSet<int>())==null,"Native profile read source equipment animations");
        Check(checkedVertices>4000&&checkedWeapons==15&&mountedVertices==checkedVertices,$"Installed equipment fixtures incomplete: vertices={checkedVertices},mounted={mountedVertices},attachments={checkedWeapons}");Directory.CreateDirectory(output);File.WriteAllText(Path.Combine(output,"equipment-regression.json"),JsonSerializer.Serialize(new{status="PASS",checkedVertices,mountedVertices,checkedWeapons,originalAssets=hashes.Count,reports},new JsonSerializerOptions{WriteIndented=true}));
        Console.WriteLine($"PASS equipment: {reports.Count} source loadouts, {checkedWeapons} authored anatomical mounts, {checkedVertices} serialized vertices; original assets unchanged");
    }
}
