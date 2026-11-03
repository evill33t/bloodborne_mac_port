// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class HavelEquipment {
    internal const int ArmorModel=8870,ToothModel=8871,ShieldModel=8872;
    internal const short RightSocket=463,LeftSocket=464;
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,SkinBake Skin,NpcAppearanceResult Appearance);
    internal static readonly IReadOnlyDictionary<string,string> Twists=new Dictionary<string,string> {
        ["LCalfTwist"]="Support_L_CalfTwist",["RCalfTwist"]="Support_R_CalfTwist",
        ["L_ForeTwist"]="Support_L_ForeArmTwist",["R_ForeTwist"]="Support_R_ForeArmTwist",
        ["LUpArmTwist"]="Support_L_UpArmTwist",["RUpArmTwist"]="Support_R_UpArmTwist"
    };
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Havel equipment: "+why);}
    internal static float BindError(Matrix4x4 a,Matrix4x4 b) {
        var d=a-b;return new[]{d.M11,d.M12,d.M13,d.M14,d.M21,d.M22,d.M23,d.M24,d.M31,d.M32,d.M33,d.M34,d.M41,d.M42,d.M43,d.M44}.Max(MathF.Abs);
    }
    internal static void CanonicalizePartBinds(FLVER2 part,FLVER2 body,FLVER2 source) {
        var canonical=body.Nodes.Select((n,i)=>(n.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        var partNames=part.Nodes.Select((n,i)=>(n.Name,i)).ToDictionary(p=>p.Name,p=>p.i,StringComparer.Ordinal);
        var bodyWorld=SkinRetarget.World(body.Nodes);var before=SkinRetarget.World(part.Nodes);
        // A retail part's disabled tail contains disconnected placeholder binds.
        // Its active skin is compatible with the shared player rig, but merely
        // enabling an unused named joint does not make its bind compatible.
        foreach(var mesh in part.Meshes)foreach(var vertex in mesh.Vertices)for(int slot=0;slot<4;slot++)if(vertex.BoneWeights[slot]>0) {
            int at=SkinRetarget.Bone(mesh,vertex.BoneIndices[slot]);
            Need((part.Nodes[at].Flags&FLVER.Node.NodeFlags.Disabled)==0&&canonical.TryGetValue(part.Nodes[at].Name,out _),"native part weights an unknown or disabled joint");
            Need(BindError(before[at],bodyWorld[canonical[part.Nodes[at].Name]])<.001f,"native part's weighted bind differs from the player rig: "+part.Nodes[at].Name);
        }
        var required=new HashSet<int>();
        foreach(var mesh in source.Meshes)foreach(var vertex in mesh.Vertices)for(int slot=0;slot<4;slot++)if(vertex.BoneWeights[slot]>0) {
            string name=source.Nodes[SkinRetarget.Bone(mesh,vertex.BoneIndices[slot])].Name;name=Twists.GetValueOrDefault(name,name);
            Need(partNames.TryGetValue(name,out int at),"source armor joint has no native part bind: "+name);
            while(at>=0&&required.Add(at))at=part.Nodes[at].ParentIndex;
        }
        var desired=part.Nodes.Select((n,i)=>required.Contains(i)&&(n.Flags&FLVER.Node.NodeFlags.Disabled)!=0&&canonical.TryGetValue(n.Name,out int at)?bodyWorld[at]:before[i]).ToArray();
        for(int i=0;i<part.Nodes.Count;i++) {
            var node=part.Nodes[i];int parent=node.ParentIndex;
            if(desired[i]==before[i]&&(parent<0||desired[parent]==before[parent]))continue;
            Need(Matrix4x4.Invert(parent<0?Matrix4x4.Identity:desired[parent],out var inverse),"singular armor parent bind");
            var local=desired[i]*inverse;
            Need(Matrix4x4.Decompose(local,out var scale,out var rotation,out var position)&&float.IsFinite(rotation.LengthSquared()),"invalid armor bind transform");
            var r=Matrix4x4.CreateFromQuaternion(rotation);float z=MathF.Asin(Math.Clamp(r.M12,-1,1));
            node.Scale=scale;node.Translation=position;
            node.Rotation=MathF.Abs(MathF.Cos(z))>.00001f?new(MathF.Atan2(-r.M32,r.M22),MathF.Atan2(-r.M13,r.M11),z):new(0,MathF.Atan2(r.M31,r.M33),z);
            Need(BindError(node.ComputeLocalTransform(),local)<.00001f,"armor bind is not representable in the native XZY layout: "+node.Name);
        }
        var after=SkinRetarget.World(part.Nodes);
        for(int i=0;i<part.Nodes.Count;i++)Need(BindError(after[i],desired[i])<.00001f,"armor bind hierarchy changed: "+part.Nodes[i].Name);
    }
    internal static Result Build(Func<string,byte[]?> source,Func<string,byte[]> native,string definitions) {
        var appearance=NpcAppearance.Load(source,definitions,6580,applyBodyShape:true);
        Need(appearance.Parts.Count==4&&appearance.Parts.All(p=>p.ModelID==9570),"source armor loadout changed");
        var body=BND4.Read(native("chr/c0000.chrbnd.dcx"));var bodyFile=body.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));var bodyModel=FLVER2.Read(bodyFile.Bytes);
        Need(bodyModel.Nodes.Count==310&&bodyModel.Meshes.Count==0,"native player body ABI changed");
        var shader=FLVER2.Read(BND4.Read(native("chr/c2530.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
        var template=FLVER2.Read(shader.Write());template.Nodes=bodyModel.Nodes;template.Dummies=[];
        var skin=SkinRetarget.Bake(appearance.Model.Write(),template.Write(),appearance.Textures,overrides:Twists,textureOwner:"c8870",sourceProfile:"DS1R",preserveBoneLengths:true);
        Need(skin.DirectWeightShare>.999999,"source armor bone has no direct player mapping");
        var files=new Dictionary<string,byte[]>();int meshOffset=0;
        foreach(var part in appearance.Parts) {
            string slot=part.Slot switch {"Head"=>"hd","Body"=>"bd","Arms"=>"am","Legs"=>"lg",_=>throw new InvalidDataException("Unexpected Havel armor slot")};
            var sourceBank=BND3.Read(source(part.Path)??throw new FileNotFoundException("Havel armor part unavailable",part.Path));
            var original=FLVER2.Read(sourceBank.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
            int count=original.Meshes.Count(m=>m.Vertices.Count>0&&m.FaceSets.Count>0);Need(count>0&&meshOffset+count<=skin.Model.Meshes.Count,"armor mesh partition mismatch");
            var model=FLVER2.Read(appearance.Model.Write());model.Meshes=model.Meshes.Skip(meshOffset).Take(count).ToList();meshOffset+=count;model.Dummies=[];
            string from=$"{slot}_m_4500",to=$"{slot}_a_{ArmorModel}";
            var carrier=BND4.Read(native("parts/"+from+".partsbnd.dcx"));
            var nativePart=FLVER2.Read(carrier.Files.Single(f=>f.ID==200).Bytes);
            CanonicalizePartBinds(nativePart,bodyModel,model);
            var partSkin=SkinRetarget.Bake(model.Write(),nativePart.Write(),appearance.Textures,overrides:Twists,textureOwner:"c8870",sourceProfile:"DS1R",preserveBoneLengths:true,playerArmor:true);
            Need(partSkin.DirectWeightShare>.999999,"armor part lacks a native support bone");model=partSkin.Model;
            foreach(var m in model.Meshes)m.NodeIndex=nativePart.Meshes[0].NodeIndex;
            // Retail apparel disables the full skeleton's unused tail. Havel's
            // armor can weight joints absent from that donor's active prefix.
            // Activate their ancestors and permute every reference together.
            var used=new HashSet<int>();
            foreach(var m in model.Meshes)foreach(var v in m.Vertices)for(int i=0;i<4;i++)if(v.BoneWeights[i]>0) {
                int at=SkinRetarget.Bone(m,v.BoneIndices[i]);while(at>=0&&used.Add(at)){model.Nodes[at].Flags&=~FLVER.Node.NodeFlags.Disabled;at=model.Nodes[at].ParentIndex;}
            }
            NonHumanoidEnemies.OrderRenderNodes(model);
            carrier.Files.RemoveAll(f=>f.ID is not(100 or 200));Need(carrier.Files.Count==2,"native armor carrier layout changed");
            foreach(var f in carrier.Files){f.Name=f.Name.Replace(from,to,StringComparison.OrdinalIgnoreCase);f.Bytes=f.ID==100?partSkin.Textures.Write():model.Write();}
            var encoded=carrier.Write(DCX.Type.DCX_DFLT_10000_44_9);files.Add("parts/"+to+".partsbnd.dcx",encoded);
            // Player NPCs request the low-detail part on their first backread.
            // Supply the same owned geometry until an authored LOD is baked.
            files.Add("parts/"+to+"_l.partsbnd.dcx",encoded.ToArray());
        }
        Need(meshOffset==skin.Model.Meshes.Count,"unassigned Havel armor meshes");
        // New attachment frames are local identity on the animated weapon
        // bones. Native hunter sockets, transforms and their indexes stay intact.
        var world=SkinRetarget.World(bodyModel.Nodes);
        foreach(var (id,name) in new[]{(RightSocket,"R_Weapon"),(LeftSocket,"L_Weapon")}) {
            Need(!bodyModel.Dummies.Any(d=>d.ReferenceID==id),"private attachment socket occupied");
            int at=bodyModel.Nodes.FindIndex(n=>n.Name==name);Need(at>=0&&at<108,"weapon bone is outside the native animated rig");var w=world[at];
            bodyModel.Dummies.Add(new(){ReferenceID=id,AttachBoneIndex=(short)at,ParentBoneIndex=(short)at,Position=w.Translation,Forward=Vector3.TransformNormal(Vector3.UnitZ,w),Upward=Vector3.TransformNormal(Vector3.UnitY,w),FollowAttachBone=true,UseUpwardVector=true,Color=System.Drawing.Color.White});
        }
        bodyFile.Bytes=bodyModel.Write();files.Add("chr/c0000.chrbnd.dcx",body.Write(DCX.Type.DCX_DFLT_10000_44_9));
        foreach(var (from,to) in new[]{(655,ToothModel),(1532,ShieldModel)}) {
            string path=$"parts/WP_A_{from:D4}.partsbnd.dcx";var original=BND3.Read(source(path)??throw new FileNotFoundException("Havel weapon unavailable",path));
            var model=FLVER2.Read(original.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)).Bytes);
            Need(model.Meshes.All(m=>!m.UseBoneWeights&&m.NodeIndex==0&&m.BoneIndices.SequenceEqual(new[]{0})&&m.Vertices.All(v=>v.NormalW==127&&v.BoneIndices[0]==0)),"source rigid weapon layout changed");
            // Compose writes an explicit weighted source layout. Merely setting
            // Dynamic on the rigid layout would drop weights during Write().
            var prepared=NpcAppearance.Compose(model,[model]);prepared.Dummies=model.Dummies;model=prepared;
            var carrier=BND4.Read(native("parts/wp_a_1501.partsbnd.dcx"));var rigid=FLVER2.Read(BND4.Read(native("parts/wp_a_1031.partsbnd.dcx")).Files.Single(f=>f.ID==200).Bytes);
            var render=FLVER2.Read(shader.Write());render.Nodes=model.Nodes;render.Dummies=model.Dummies;
            var exact=model.Nodes.ToDictionary(n=>n.Name,n=>n.Name,StringComparer.Ordinal);var textures=original.Files.Where(f=>TPF.Is(f.Bytes.ToArray())).SelectMany(f=>TPF.Read(f.Bytes).Textures).ToArray();
            var weapon=SkinRetarget.Bake(model.Write(),render.Write(),textures,overrides:exact,textureOwner:"c"+to,sourceProfile:"DS1R",preserveBoneLengths:true);
            // The conversion intermediate is skinned, while the actual retail
            // weapon is rigid. Restore its real shader, GX, vertex layout and
            // mesh transform contract; no invented skin inputs reach runtime.
            var rigidMesh=rigid.Meshes[0];var rigidMaterial=rigid.Materials[rigidMesh.MaterialIndex];
            Need(!rigidMesh.UseBoneWeights&&Leaf(rigidMaterial.MTD)=="P[Weapon]_[ARSN]_NoCM.mtd"&&model.Nodes[0].ComputeLocalTransform()==Matrix4x4.Identity,"rigid weapon template changed");
            weapon.Model.BufferLayouts=rigid.BufferLayouts;weapon.Model.GXLists=rigid.GXLists;
            foreach(var material in weapon.Model.Materials) {
                var converted=material.Textures.ToDictionary(t=>t.ParamName,StringComparer.Ordinal);material.MTD=rigidMaterial.MTD;material.GXIndex=rigidMaterial.GXIndex;
                material.Textures=rigidMaterial.Textures.Select(t=>{var from=converted.GetValueOrDefault(t.ParamName)??t;return new FLVER2.Texture(from.ParamName,from.Path,from.TilingScale,from.TilingTypeU,from.TilingTypeV,from.Unk14,from.Unk18,from.Unk1C);}).ToList();
            }
            foreach(var m in weapon.Model.Meshes){m.NodeIndex=0;m.UseBoneWeights=false;m.VertexBuffers=rigidMesh.VertexBuffers.Select(b=>new FLVER2.VertexBuffer(b.LayoutIndex){BufferIndex=b.BufferIndex}).ToList();foreach(var v in m.Vertices)v.NormalW=rigidMesh.Vertices[0].NormalW;}
            Need(weapon.DirectWeightShare>.999999&&weapon.Model.Dummies.Count(d=>d.ReferenceID==130)==1,"source weapon skin/contact socket changed");
            carrier.Files.RemoveAll(f=>f.ID is not(100 or 200));Need(carrier.Files.Count==2,"native shield carrier layout changed");
            foreach(var f in carrier.Files){f.Name=f.Name.Replace("WP_A_1501","WP_A_"+to,StringComparison.OrdinalIgnoreCase);f.Bytes=f.ID==100?weapon.Textures.Write():weapon.Model.Write();}
            var encoded=carrier.Write(DCX.Type.DCX_DFLT_10000_44_9);files.Add($"parts/wp_a_{to}.partsbnd.dcx",encoded);files.Add($"parts/wp_a_{to}_l.partsbnd.dcx",encoded.ToArray());
        }
        return new(files,skin,appearance);
    }
}
