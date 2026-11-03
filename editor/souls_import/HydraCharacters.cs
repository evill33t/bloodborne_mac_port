// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class HydraCharacters {
    internal const string Model="c8830",NativeModel="c2730";
    internal const int SocketBase=500;
    internal static int Socket(int sourceId) {
        if(sourceId<0||sourceId>=500)throw new InvalidDataException("Hydra source socket outside body lookup namespace");return SocketBase+sourceId;
    }
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,HydraPhysicsRig.Report PhysicsReport,HydraPwv.Report PwvReport,
        HydraAnimations.Report AnimationReport,HydraParameters.Report ParameterReport,HydraParameters.Ids Ids,IReadOnlyList<int> RequiredNativeSfx,IReadOnlyList<string> Limitations,SkinBake Skin);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Hydra character: "+why);}
    internal static Result Bake(Func<string,byte[]?> sourceRead,Func<string,byte[]> nativeRead,Func<string,byte[]> composedRead,string definitions) {
        var source=MapCharacters.Authored(sourceRead,"c3530");var sourceModel=FLVER2.Read(source.Model);var actions=new SourceCharacterActions(sourceRead,"c3530");
        var character=BND4.Read(nativeRead("chr/c2730.chrbnd.dcx"));var animation=BND4.Read(nativeRead("chr/c2730.anibnd.dcx"));
        var visual=character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));var skeleton=animation.Files.Single(f=>f.ID==1000000);
        NativeRigAppend.ValidateCharacter(animation.Files.Single(f=>f.ID==7000400).Bytes.ToArray());
        var plan=NonHumanoidEnemies.Rig(source.Model,visual.Bytes.ToArray(),skeleton.Bytes.ToArray(),actions.Resolve(0).Clip);
        Need(plan.Count==356&&plan.Added.Count==246,"complete source visual/controller rig required");
        var physicsSource=HydraPhysicsSource.Load(sourceRead);var physics=HydraPhysicsRig.Convert(character.Files.Single(f=>f.ID==300).Bytes.ToArray(),physicsSource,plan);
        var pwvFile=character.Files.Single(f=>Leaf(f.Name).EndsWith(".hkxpwv"));var pwv=HydraPwv.Convert(pwvFile.Bytes.ToArray(),physicsSource,plan,HydraParameters.RagdollIds);
        var skinTemplate=FLVER2.Read(BND4.Read(nativeRead("chr/c2530.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
        var render=FLVER2.Read(plan.Model);render.BufferLayouts=skinTemplate.BufferLayouts;render.GXLists=skinTemplate.GXLists;render.Materials=skinTemplate.Materials;render.Meshes=skinTemplate.Meshes;
        var overrides=plan.Added.ToDictionary(b=>b.Name[4..],b=>b.Name,StringComparer.Ordinal);
        var skin=SkinRetarget.Bake(source.Model,render.Write(),source.Textures,overrides:overrides,textureOwner:Model,sourceProfile:"DS1R",preserveBoneLengths:true);
        Need(skin.DirectWeightShare>.999999&&skin.Model.Meshes.Count==26&&skin.Model.Meshes.Count==sourceModel.Meshes.Count&&skin.Model.Meshes.All(m=>SkinRetarget.MaximumBoneIndex(skin.Model,m)==ushort.MaxValue),"source geometry/UShort4 render contract changed");
        // Material names encode model display masks; do not leave the generic
        // Imported_N labels that would prevent individual heads disappearing.
        for(int i=0;i<26;i++)skin.Model.Materials[skin.Model.Meshes[i].MaterialIndex].Name=sourceModel.Materials[sourceModel.Meshes[i].MaterialIndex].Name;
        var sockets=sourceModel.Dummies.Select(d=>(int)d.ReferenceID).ToHashSet();Need(sockets.All(id=>id is >=0 and <500),"out-of-range source sockets");
        var nativeSockets=skin.Model.Dummies.Select(d=>d.ReferenceID).ToHashSet();
        Need(!sockets.Any(id=>nativeSockets.Contains(checked((short)Socket(id)))),"native/source socket namespace overlap");
        var originalWorld=SkinRetarget.World(sourceModel.Nodes);var targetModel=FLVER2.Read(plan.Model);var targetWorld=SkinRetarget.World(targetModel.Nodes);
        int Owner(int index) {var visited=new HashSet<int>();while(index>=0){Need(index<sourceModel.Nodes.Count&&visited.Add(index),"invalid source socket ancestry");if(overrides.ContainsKey(sourceModel.Nodes[index].Name))return index;index=sourceModel.Nodes[index].ParentIndex;}return sourceModel.Nodes.FindIndex(n=>n.Name=="Master");}
        FLVER.Dummy Rebind(FLVER.Dummy original,short reference) {
            int owner=Owner(original.ParentBoneIndex);Need(owner>=0,"missing source socket owner");Need(Matrix4x4.Invert(originalWorld[owner],out var inverse),"singular source socket owner");
            int parent=targetModel.Nodes.FindIndex(n=>n.Name=="DS1_"+sourceModel.Nodes[owner].Name);Need(parent>=0,"source socket owner omitted from appended rig");
            var before=original.ParentBoneIndex<0?Matrix4x4.Identity:originalWorld[original.ParentBoneIndex];var delta=before*inverse;
            var result=new FLVER.Dummy(original){ReferenceID=reference,ParentBoneIndex=checked((short)parent),Position=Vector3.Transform(original.Position,delta),Forward=Vector3.TransformNormal(original.Forward,delta),Upward=Vector3.TransformNormal(original.Upward,delta)};
            if(original.AttachBoneIndex>=0){int attach=Owner(original.AttachBoneIndex),mapped=targetModel.Nodes.FindIndex(n=>n.Name=="DS1_"+sourceModel.Nodes[attach].Name);Need(mapped>=0,"source socket attach owner omitted");result.AttachBoneIndex=checked((short)mapped);}
            Need(Vector3.Distance(Vector3.Transform(original.Position,before),Vector3.Transform(result.Position,targetWorld[parent]))<.001f,"source socket bind frame changed");return result;
        }
        foreach(var dummy in sourceModel.Dummies) {
            short id=checked((short)Socket(dummy.ReferenceID));
            skin.Model.Dummies.Add(Rebind(dummy,id));
        }
        NonHumanoidEnemies.OrderRenderNodes(skin.Model);visual.Bytes=skin.Model.Write();skeleton.Bytes=plan.Skeleton;
        character.Files.Single(f=>f.ID==300).Bytes=physics.Physics;pwvFile.Bytes=pwv.Pwv;
        var parameters=HydraParameters.Build(composedRead("param/gameparam/gameparam.parambnd.dcx"),sourceRead,definitions,actions,sockets);
        var animationReport=HydraAnimations.Apply(animation,plan,actions,BND4.Read(nativeRead("chr/c2630.anibnd.dcx")));
        character.Files.RemoveAll(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name)=="div_data.txt"||TPF.Is(f.Bytes.ToArray()));
        Need(!character.Files.Any(f=>f.ID==100),"texture binder reservation occupied");
        character.Files.Add(new BinderFile(Binder.FileFlags.Flag1,100,@"N:\SPRJ\data\INTERROOT_ps4\chr\c8830\c8830.tpf",skin.Textures.Write(DCX.Type.None)));
        foreach(var f in character.Files)f.Name=f.Name.Replace(NativeModel,Model,StringComparison.OrdinalIgnoreCase);
        foreach(var f in animation.Files) {
            f.Name=f.Name.Replace(NativeModel,Model,StringComparison.OrdinalIgnoreCase);
            if(TAE.Is(f.Bytes.ToArray())){var tae=TAE.Read(f.Bytes);tae.ID=208830;f.Bytes=tae.Write();}
            else if(f.ID is >=7000000 and <8000000)f.Bytes=SkinRetarget.RenameBehavior(f.Bytes,NativeModel,Model);
        }
        character.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));animation.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));
        var files=new Dictionary<string,byte[]> {
            ["chr/c8830.chrbnd.dcx"]=character.Write(DCX.Type.DCX_DFLT_10000_44_9),["chr/c8830.anibnd.dcx"]=animation.Write(DCX.Type.DCX_DFLT_10000_44_9),
            ["action/script/c8830.hks"]=nativeRead("action/script/c2730.hks"),["param/gameparam/gameparam.parambnd.dcx"]=parameters.Game};
        return new(files,physics.Report,pwv.Report,animationReport,parameters.Report,parameters.Ids,HydraParameters.RequiredNativeSfx,[
            "The complete original Hydra body, all seven display-mask head groups, source contact/projectile/sever sockets and original97 capsule hurtboxes are retained. Native controller and shader/material response remain compatibility carriers.",
            "Map integration must add owned source AI, seven detached head actors/sever events, reward/quest persistence and the complete native625306/307 SFX resource closure before enabling the encounter. This package fixture does not itself prove native combat, severing or save behavior."],skin);
    }
}
