// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class GoldenGolemCharacters {
    internal const string Model="c8832",NativeModel="c2730";
    internal const int SocketBase=500;
    internal static int Socket(int sourceId) {
        if(sourceId<0||sourceId>=500)throw new InvalidDataException("Golden source socket outside body lookup namespace");return SocketBase+sourceId;
    }
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,GoldenGolemPhysics.Report PhysicsReport,
        GoldenGolemAnimations.Report AnimationReport,GoldenGolemParameters.Report ParameterReport,GoldenGolemParameters.Ids Ids,IReadOnlyList<int> RequiredNativeSfx,IReadOnlyList<string> Limitations,SkinBake Skin);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Golden golem character: "+why);}
    internal static Result Bake(Func<string,byte[]?> sourceRead,Func<string,byte[]> nativeRead,Func<string,byte[]> composedRead,string definitions) {
        var source=MapCharacters.Authored(sourceRead,"c2711");var sourceModel=FLVER2.Read(source.Model);var actions=new SourceCharacterActions(sourceRead,"c2711");
        var character=BND4.Read(nativeRead("chr/c2730.chrbnd.dcx"));var animation=BND4.Read(nativeRead("chr/c2730.anibnd.dcx"));
        var visual=character.Files.Single(f=>Leaf(f.Name).EndsWith(".flver",StringComparison.OrdinalIgnoreCase));var skeleton=animation.Files.Single(f=>f.ID==1000000);
        NativeRigAppend.ValidateCharacter(animation.Files.Single(f=>f.ID==7000400).Bytes.ToArray());
        var plan=NonHumanoidEnemies.Rig(source.Model,visual.Bytes.ToArray(),skeleton.Bytes.ToArray(),actions.Resolve(0).Clip);
        Need(plan.Count==139&&plan.Added.Count==29,"complete source visual/controller rig required");
        var physicsSource=GoldenGolemPhysicsSource.Load(sourceRead);var physics=GoldenGolemPhysics.Convert(character.Files.Single(f=>f.ID==300).Bytes.ToArray(),physicsSource,plan);
        var pwvFile=character.Files.Single(f=>Leaf(f.Name).EndsWith(".hkxpwv"));var pwv=GoldenGolemPhysics.Pwv(pwvFile.Bytes.ToArray(),physicsSource,plan);
        var skinTemplate=FLVER2.Read(BND4.Read(nativeRead("chr/c2530.chrbnd.dcx")).Files.Single(f=>Leaf(f.Name).EndsWith(".flver")).Bytes);
        var render=FLVER2.Read(plan.Model);render.BufferLayouts=skinTemplate.BufferLayouts;render.GXLists=skinTemplate.GXLists;render.Materials=skinTemplate.Materials;render.Meshes=skinTemplate.Meshes;
        var overrides=plan.Added.ToDictionary(b=>b.Name[4..],b=>b.Name,StringComparer.Ordinal);
        var skin=SkinRetarget.Bake(source.Model,render.Write(),source.Textures,overrides:overrides,textureOwner:Model,sourceProfile:"DS1R",preserveBoneLengths:true);
        Need(skin.DirectWeightShare>.999999&&skin.Model.Meshes.Count==sourceModel.Meshes.Count&&skin.Model.Meshes.All(m=>SkinRetarget.MaximumBoneIndex(skin.Model,m)==ushort.MaxValue),"source geometry/UShort4 render contract changed");
        // Preserve original material/mask identities while using native shaders.
        for(int i=0;i<sourceModel.Meshes.Count;i++)skin.Model.Materials[skin.Model.Meshes[i].MaterialIndex].Name=sourceModel.Materials[sourceModel.Meshes[i].MaterialIndex].Name;
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
        character.Files.Single(f=>f.ID==300).Bytes=physics.Physics;pwvFile.Bytes=pwv;
        var parameters=GoldenGolemParameters.Build(composedRead("param/gameparam/gameparam.parambnd.dcx"),sourceRead,definitions,actions,sockets);
        var animationReport=GoldenGolemAnimations.Apply(animation,plan,actions,BND4.Read(nativeRead("chr/c2630.anibnd.dcx")),sourceRead);
        character.Files.RemoveAll(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name)=="div_data.txt"||TPF.Is(f.Bytes.ToArray()));
        Need(!character.Files.Any(f=>f.ID==100),"texture binder reservation occupied");
        character.Files.Add(new BinderFile(Binder.FileFlags.Flag1,100,@"N:\SPRJ\data\INTERROOT_ps4\chr\c8832\c8832.tpf",skin.Textures.Write(DCX.Type.None)));
        foreach(var f in character.Files)f.Name=f.Name.Replace(NativeModel,Model,StringComparison.OrdinalIgnoreCase);
        foreach(var f in animation.Files) {
            f.Name=f.Name.Replace(NativeModel,Model,StringComparison.OrdinalIgnoreCase);
            if(TAE.Is(f.Bytes.ToArray())){var tae=TAE.Read(f.Bytes);tae.ID=208832;f.Bytes=tae.Write();}
            else if(f.ID is >=7000000 and <8000000)f.Bytes=SkinRetarget.RenameBehavior(f.Bytes,NativeModel,Model);
        }
        character.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));animation.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));
        var files=new Dictionary<string,byte[]> {
            ["chr/c8832.chrbnd.dcx"]=character.Write(DCX.Type.DCX_DFLT_10000_44_9),["chr/c8832.anibnd.dcx"]=animation.Write(DCX.Type.DCX_DFLT_10000_44_9),
            ["action/script/c8832.hks"]=nativeRead("action/script/c2730.hks"),["param/gameparam/gameparam.parambnd.dcx"]=parameters.Game};
        return new(files,physics.Report,animationReport,parameters.Report,parameters.Ids,GoldenGolemParameters.RequiredNativeSfx,[
            "Original golden golem geometry, all29source bones,18capsule hurtboxes,17hinges,9attacks, source root motion and Dusk rescue socket101→601 are retained. Native graph and shader response are compatibility carriers.",
            "The owned sourceAI and Dusk quest presence/death events must be composed by map integration. Source crystal impact uses a native fluid/arcane impact visual pending originalFXR translation; source chained ballistics and damage remain authored."],skin);
    }
}
