// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// Source appearance and placements on a friendly native animation wrapper with AI disabled.
// Source animation, dialogue, equipment behavior and quest events are not ported.
internal static class MapCharacters {
    internal const string TemplateModel="c8030";
    internal const int TemplateParam=803000;
    internal record CharacterReport(string SourceName,string SourceModel,int SourceEntityID,int CharaInitID,
        int SourceNPCParamID,int SourceThinkParamID,int SourceTalkID,int SourceInitAnimID,
        Vector3 SourcePosition,Vector3 SourceRotation,Vector3 SourceScale,string Status,string? TargetModel,
        int? EntityID,int? NPCParamID,int? ThinkParamID,int Vertices,int Triangles,double DirectWeightShare,
        IReadOnlyList<string> SourceParts,IReadOnlyList<string> Limitations,IReadOnlyList<string> TextureFallbacks);
    internal record Export(IReadOnlyDictionary<string,byte[]> Files,IReadOnlyList<MSBB.Model.Enemy> Models,
        IReadOnlyList<MSBB.Part.Enemy> Actors,IReadOnlyList<CharacterReport> Reports,byte[] GameParamBytes);

    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    static string TextureName(string path)=>Leaf(path).Split('.')[0];
    static bool Finite(Vector3 p)=>float.IsFinite(p.X)&&float.IsFinite(p.Y)&&float.IsFinite(p.Z);
    static byte[] Required(Func<string,byte[]?> read,string path)=>read(path)??throw new FileNotFoundException("Source character asset unavailable",path);
    static List<BinderFile> Binder(byte[] bytes) {
        if(bytes.Length>512*1024*1024)throw new InvalidDataException("Source character binder exceeds the import budget");
        if(DCX.Is(bytes))bytes=DCX.Decompress(bytes).ToArray();
        return BND3.Is(bytes)?BND3.Read(bytes).Files:BND4.Is(bytes)?BND4.Read(bytes).Files:throw new InvalidDataException("Unsupported source character binder");
    }
    static bool IsModel(string path)=>Leaf(path).EndsWith(".flver",StringComparison.OrdinalIgnoreCase)||Leaf(path).EndsWith(".flver.dcx",StringComparison.OrdinalIgnoreCase);

    // The original native row bytes, padding, duplicate rows and row order remain
    // opaque. Only the two documented clone fields below are edited.
    internal static PARAM NativeRows(Memory<byte> bytes,bool think) {
        var rows=PARAM.Read(bytes);string type=think?"NPC_THINK_PARAM_ST":"NPC_PARAM_ST";short version=(short)(think?1:5);int size=think?148:388;
        if(rows.ParamType!=type||rows.ParamdefDataVersion!=version||rows.DetectedSize!=size||rows.BigEndian||rows.Format2D!=PARAM.FormatFlags1.LongDataOffset)
            throw new InvalidDataException("Unsupported native "+(think?"NpcThinkParam":"NpcParam")+" layout");
        var def=new PARAMDEF{ParamType=type,DataVersion=version,Unicode=true};
        def.Fields.Add(new PARAMDEF.Field(def,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});rows.ApplyParamdef(def);return rows;
    }
    internal static byte[] Payload(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();
    static byte[] WriteRows(PARAM rows) {
        // SoulsFormats normally normalizes null names to empty strings. Keep the
        // original null name offsets in Bloodborne's 64-bit row-header layout.
        var nulls=rows.Rows.Select((row,index)=>(row.Name,index)).Where(p=>p.Name==null).Select(p=>p.index).ToArray();
        var bytes=rows.Write();foreach(int i in nulls)Array.Clear(bytes,64+i*24+16,8);return bytes;
    }
    static void CloneRow(PARAM rows,PARAM.Row donor,int id,string name,byte[] payload) {
        if(rows.Rows.Any(r=>r.ID==id))throw new InvalidDataException("Character parameter ID collision");
        var row=new PARAM.Row(donor){ID=id,Name=name};row.Cells.Single().Value=payload.ToArray();rows.Rows.Add(row);
    }

    static (byte[] Model,List<TPF.Texture> Textures,List<string> Paths) Authored(Func<string,byte[]?> read,string model) {
        string sourcePath=$"chr/{model}.chrbnd.dcx";var files=Binder(read(sourcePath)??Required(read,$"chr/{model}.chrbnd"));
        var visual=files.SingleOrDefault(f=>IsModel(f.Name))??throw new InvalidDataException("Authored source character has no unique FLVER");
        if(!FLVER2.Is(visual.Bytes.ToArray()))throw new InvalidDataException("Authored source character requires FLVER2");
        var textures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);var paths=new List<string>{sourcePath};
        void Add(TPF tpf){foreach(var texture in tpf.Textures) {
            string name=TextureName(texture.Name);if(textures.TryGetValue(name,out var previous)) {
                if(previous.Format!=texture.Format||!previous.Bytes.AsSpan().SequenceEqual(texture.Bytes))throw new InvalidDataException("Conflicting authored character texture: "+name);
            } else textures.Add(name,texture);
        }}
        foreach(var file in files)if(TPF.Is(file.Bytes.ToArray()))Add(TPF.Read(file.Bytes));
        foreach(var header in files.Where(f=>Leaf(f.Name).EndsWith(".chrtpfbhd",StringComparison.OrdinalIgnoreCase))) {
            string dataPath=$"chr/{model}.chrtpfbdt";var data=Required(read,dataPath);paths.Add(dataPath);
            using var split=new BXF3Reader(header.Bytes,data);
            foreach(var file in split.Files) {
                if(file.CompressedSize>128L*1024*1024||file.UncompressedSize>128L*1024*1024)throw new InvalidDataException("Character texture exceeds the import budget");
                Add(TPF.Read(split.ReadFile(file)));
            }
        }
        foreach(var path in new[]{$"chr/{model}.texbnd.dcx",$"chr/{model}.texbnd"})if(read(path) is {} bytes) {
            paths.Add(path);foreach(var file in Binder(bytes))if(TPF.Is(file.Bytes.ToArray()))Add(TPF.Read(file.Bytes));
        }
        foreach(var suffix in new[]{"_2.tpf.dcx",".tpf.dcx",".tpf"})if(read($"chr/{model}{suffix}") is {} bytes) {
            paths.Add($"chr/{model}{suffix}");Add(TPF.Read(bytes));
        }
        // Anastacia's authored mesh reuses the original common female face map.
        foreach(var path in new[]{"parts/Common_Body.tpf","parts/Common_Body_M.tpf"})if(read(path) is {} bytes) {
            paths.Add(path);foreach(var texture in TPF.Read(bytes).Textures)if(!textures.ContainsKey(TextureName(texture.Name)))textures.Add(TextureName(texture.Name),texture);
        }
        var flver=FLVER2.Read(visual.Bytes);
        foreach(var material in flver.Materials) {
            var diffuse=material.Textures.FirstOrDefault(t=>t.ParamName is "g_Diffuse" or "g_DiffuseTexture");
            if(diffuse==null||!textures.ContainsKey(TextureName(diffuse.Path)))throw new InvalidDataException("Authored character diffuse texture is unavailable: "+(diffuse?.Path??material.Name));
        }
        return(visual.Bytes.ToArray(),textures.Values.ToList(),paths);
    }

    // Pure in-memory staging: callers compose the returned map entries and files
    // with their other import stages before writing a mod outside either game.
    internal static Export BakeFirelink(Func<string,byte[]?> readSource,string profile,string sourceMap,string definitionsDirectory,
        string nativeDvdRoot,MSBB destination,Vector3 offset,string collisionName,byte[]? gameParamOverride=null) {
        if(profile is not("DS1" or "DS1R")||sourceMap!="m10_02_00_00")throw new NotSupportedException("Character actor import currently supports DS1 Firelink only");
        if(!Finite(offset)||destination.Parts.Collisions.Count(c=>c.Name==collisionName)!=1)throw new InvalidDataException("Character import requires a valid offset and destination collision");
        string msbPath=$"map/MapStudio/{sourceMap}.msb";var source=MSB1.Read(readSource(msbPath)??Required(readSource,msbPath+".dcx"));
        var selected=new[]{
            source.Parts.Enemies.Single(p=>p.ModelName=="c0000"&&p.CharaInitID==6270),
            source.Parts.Enemies.Single(p=>p.ModelName=="c0000"&&p.CharaInitID==6080),
            source.Parts.Enemies.Single(p=>p.ModelName=="c2750"&&p.NPCParamID==275000)};
        foreach(var p in selected)if(!Finite(p.Position)||!Finite(p.Rotation)||!Finite(p.Scale)||
            Math.Min(p.Scale.X,Math.Min(p.Scale.Y,p.Scale.Z))<=0||Math.Max(p.Scale.X,Math.Max(p.Scale.Y,p.Scale.Z))>10)
            throw new InvalidDataException("Invalid source character transform");
        var donorMap=MSBB.Read(File.ReadAllBytes(Path.Combine(nativeDvdRoot,"map/MapStudio/m21_00_00_00.msb.dcx")));
        var donor=donorMap.Parts.Enemies.Single(p=>p.ModelName==TemplateModel&&p.NPCParamID==TemplateParam&&p.ThinkParamID==TemplateParam);
        var donorModel=donorMap.Models.Enemies.Single(m=>m.Name==TemplateModel);
        var nativeCharacter=File.ReadAllBytes(Path.Combine(nativeDvdRoot,"chr",TemplateModel+".chrbnd.dcx"));
        var nativeAnimation=File.ReadAllBytes(Path.Combine(nativeDvdRoot,"chr",TemplateModel+".anibnd.dcx"));
        var nativeScript=File.ReadAllBytes(Path.Combine(nativeDvdRoot,"action/script",TemplateModel+".hks"));
        var gameParams=BND4.Read(gameParamOverride??File.ReadAllBytes(Path.Combine(nativeDvdRoot,"param/gameparam/gameparam.parambnd.dcx")));
        var npcFile=gameParams.Files.Single(f=>Leaf(f.Name)=="NpcParam.param");
        var npcs=NativeRows(npcFile.Bytes,false);
        var npcDonor=npcs.Rows.Single(r=>r.ID==TemplateParam);
        var npcBytes=Payload(npcDonor);if(BitConverter.ToInt32(npcBytes,4)!=TemplateParam)throw new InvalidDataException("Native Doll AI parameter does not match its actor");
        var usedModels=destination.Models.GetEntries().Select(m=>m.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var usedNames=destination.Parts.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var usedEntityIDs=destination.Parts.GetEntries().Select(p=>p.EntityID).Concat(destination.Regions.GetEntries().Select(r=>r.EntityID)).Concat(destination.Events.GetEntries().Select(e=>e.EntityID)).ToHashSet();
        var usedParams=npcs.Rows.Select(r=>r.ID).ToHashSet();
        var files=new Dictionary<string,byte[]>();var models=new List<MSBB.Model.Enemy>();var actors=new List<MSBB.Part.Enemy>();var reports=new Dictionary<string,CharacterReport>();int nextModel=9800,nextEntity=2419700;
        foreach(var p in selected) {
            bool Occupied(int id) {
                string name="c"+id;return usedModels.Contains(name)||usedNames.Contains(name+"_0000")||usedParams.Contains(id*100)||
                    File.Exists(Path.Combine(nativeDvdRoot,"chr",name+".chrbnd.dcx"))||File.Exists(Path.Combine(nativeDvdRoot,"chr",name+".anibnd.dcx"))||File.Exists(Path.Combine(nativeDvdRoot,"action/script",name+".hks"));
            }
            while(nextModel<=9999&&Occupied(nextModel))++nextModel;if(nextModel>9999)throw new InvalidDataException("No unused character model ID");
            int id=nextModel++,paramID=id*100;string target="c"+id;usedModels.Add(target);usedParams.Add(paramID);
            while(nextEntity<=2419799&&usedEntityIDs.Contains(nextEntity))++nextEntity;if(nextEntity>2419799)throw new InvalidDataException("No unused imported character entity ID");
            int entityID=nextEntity++;usedEntityIDs.Add(entityID);
            byte[] sourceModel;List<TPF.Texture> textures;List<string> parts;List<string> limitations;
            if(p.ModelName=="c0000") {
                var appearance=NpcAppearance.Load(readSource,definitionsDirectory,p.CharaInitID);
                if(!appearance.Parts.Any(part=>part.Slot=="Face")||!appearance.Parts.Any(part=>part.Slot=="Hair"))throw new InvalidDataException("Initial Firelink human NPC requires its real source FaceGen head and hair");
                sourceModel=appearance.Model.Write();textures=appearance.Textures;parts=appearance.Parts.Select(part=>part.Path).Distinct().ToList();limitations=appearance.Limitations.ToList();
            } else {
                var appearance=Authored(readSource,p.ModelName);sourceModel=appearance.Model;textures=appearance.Textures;parts=appearance.Paths;
                limitations=["Authored source c2750 appearance retained; source cloth simulation and source shader behavior are not reproduced."];
            }
            var character=BND4.Read(nativeCharacter);var visual=character.Files.Single(f=>IsModel(f.Name));
            var bake=SkinRetarget.Bake(sourceModel,visual.Bytes,textures,masks:null,textureOwner:target,sourceProfile:profile);visual.Bytes=bake.Model.Write();
            character.Files.RemoveAll(f=>Leaf(f.Name).EndsWith("_c.hkx",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).EndsWith(".clm2",StringComparison.OrdinalIgnoreCase)||Leaf(f.Name).Equals("div_data.txt",StringComparison.OrdinalIgnoreCase)||TPF.Is(f.Bytes.ToArray()));
            if(character.Files.Any(f=>f.ID==100))throw new InvalidDataException("Native actor template reserves texture binder ID 100");
            character.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,100,@"N:\SPRJ\data\INTERROOT_ps4\chr\"+target+@"\"+target+".tpf",bake.Textures.Write(DCX.Type.None)));
            foreach(var file in character.Files)file.Name=file.Name.Replace(TemplateModel,target,StringComparison.OrdinalIgnoreCase);
            character.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));files.Add("chr/"+target+".chrbnd.dcx",character.Write(DCX.Type.DCX_DFLT_10000_44_9));
            var animation=BND4.Read(nativeAnimation);
            foreach(var file in animation.Files) {
                file.Name=file.Name.Replace(TemplateModel,target,StringComparison.OrdinalIgnoreCase);
                if(TAE.Is(file.Bytes.ToArray())){var tae=TAE.Read(file.Bytes);tae.ID=200000+id;file.Bytes=tae.Write();}
                else if(file.ID>=7000000&&file.ID<8000000)file.Bytes=SkinRetarget.RenameBehavior(file.Bytes,TemplateModel,target);
            }
            files.Add("chr/"+target+".anibnd.dcx",animation.Write(DCX.Type.DCX_DFLT_10000_44_9));files.Add("action/script/"+target+".hks",nativeScript.ToArray());
            // The original Doll logic is area-local and changes native quest flags.
            // An AI-disabled actor still owns its native animation/behavior rig.
            var param=npcBytes.ToArray();BitConverter.GetBytes(-1).CopyTo(param,4);
            // Native NpcParam display-mask fields 0..31 occupy these four bytes.
            // All source material groups, including source #33, remain baked.
            foreach(int index in new[]{0x146,0x147,0x14A,0x14B})param[index]=255;
            CloneRow(npcs,npcDonor,paramID,"Imported Firelink "+p.Name,param);
            var model=(MSBB.Model.Enemy)donorModel.DeepCopy();model.Name=target;model.SibPath=model.SibPath.Replace(TemplateModel,target,StringComparison.OrdinalIgnoreCase);models.Add(model);
            var actor=(MSBB.Part.Enemy)donor.DeepCopy();actor.Name=target+"_0000";actor.ModelName=target;actor.EntityID=entityID;actor.NPCParamID=paramID;actor.ThinkParamID=-1;
            actor.TalkID=-1;actor.CharaInitID=-1;actor.Position=p.Position+offset;actor.Rotation=p.Rotation;actor.Scale=p.Scale;actor.CollisionName=collisionName;actor.PatrolInfoName=null!;
            Array.Clear(actor.MovePointNames);actor.InitAnimID=-1;actor.DamageAnimID=-1;actor.PatrolType=0;actor.FallbackPlatoonID=0;
            Array.Fill(actor.DrawGroups,uint.MaxValue);Array.Fill(actor.DispGroups,uint.MaxValue);Array.Fill(actor.BackreadGroups,uint.MaxValue);usedNames.Add(actor.Name);actors.Add(actor);
            limitations.Add("Friendly native Bloodborne Doll animation, hitbox and physics are an approximation. AI is explicitly disabled (ThinkParamID and NPC aiThinkId -1); no native Doll AI or quest logic is packaged. Source idle poses, combat, weapons, dialogue and quest events are not ported. Talk and source quest interactions are disabled. Runtime animation/appearance require gameplay validation.");
            reports.Add(p.Name,Report(p,"placed-native-idle-ai-disabled",target,entityID,paramID,bake.Vertices,bake.Triangles,bake.DirectWeightShare,parts,limitations,bake.TextureFallbacks));
        }
        npcFile.Bytes=WriteRows(npcs);var paramBundle=gameParams.Write(DCX.Type.DCX_DFLT_10000_44_9);files.Add("param/gameparam/gameparam.parambnd.dcx",paramBundle);
        foreach(var p in source.Parts.Enemies.Where(p=>!reports.ContainsKey(p.Name))) {
            string status=p.ModelName=="c0000"||p.TalkID>0?"disabled-source-quest-or-interaction":"unconverted-source-character";
            reports.Add(p.Name,Report(p,status,null,null,null,0,0,0,[],["Source actor is not instantiated. Source availability flags, AI, dialogue and quest conditions require a separate event port."],[]));
        }
        return new(files,models,actors,source.Parts.Enemies.Select(p=>reports[p.Name]).ToArray(),paramBundle);
    }
    static CharacterReport Report(MSB1.Part.Enemy p,string status,string? target,int? entity,int? param,int vertices,int triangles,double share,
        IReadOnlyList<string> parts,IReadOnlyList<string> limitations,IReadOnlyList<string> fallbacks)=>
        new(p.Name,p.ModelName,p.EntityID,p.CharaInitID,p.NPCParamID,p.ThinkParamID,p.TalkID,p.InitAnimID,p.Position,p.Rotation,p.Scale,status,target,entity,param,target==null?null:-1,vertices,triangles,share,parts,limitations,fallbacks);
}
