// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Security.Cryptography;
using System.Text.Json;
string root=Directory.Exists(Path.Combine(args[0],"dvdroot_ps4"))?Path.Combine(args[0],"dvdroot_ps4"):args[0];var hashes=new Dictionary<string,byte[]>();var files=new Dictionary<string,byte[]>();byte[]? previousEnvironment=null;
void Check(bool v,string m){if(!v)throw new Exception(m);}
byte[]? Read(string p){if(files.TryGetValue(p,out var b))return b;if(!File.Exists(Path.Combine(root,p)))return null;var bytes=File.ReadAllBytes(Path.Combine(root,p));hashes.TryAdd(p,SHA256.HashData(bytes));return bytes;}
void Write(string p,byte[] b){Check(!p.StartsWith("map/MapStudio/"),"Seed overwrote native MSB");files[p]=b;}
foreach(var (source,target) in new[]{("m10_02_00_00","m40_02_00_00"),("m10_01_00_00","m40_03_00_00")}){
 var choice=MapDestination.Select("additional","DS1R",source,target);var donor=MSBB.Read(Read("map/MapStudio/"+choice.TemplateMap+".msb.dcx")!);var before=donor.Write();var map=MapDestination.Create(donor,choice);
 Check(MSBB.Read(map.Write()).Parts.Objects.Count==map.Parts.Objects.Count,"Temporary actor lacks a serializable model reference");
 Check(map.Parts.Players.Count==1&&map.Parts.Collisions.Count==1&&map.Parts.MapPieces.Count==0&&map.Parts.Enemies.Count==0&&map.Events.GetEntries().Count==0&&map.Regions.GetEntries().Count==0,"Additional scene imported native residents/events");
 MapDestination.Seed(choice,Read,Write);var script=EMEVD.Read(files["event/"+target+".emevd.dcx"]);Check(script.Events.Select(e=>e.ID).Order().SequenceEqual(new long[]{0,50})&&script.Events.All(e=>e.Instructions.Count==0),"Fresh event roots changed");
 var oldEnv=TPF.Read(Read("map/m24/m24_envmap.tpf.dcx")!);var env=TPF.Read(files["map/m40/m40_envmap.tpf.dcx"]);
 string sourceBlock=source=="m10_02_00_00"?"m24_01":"m24_00",targetBlock=source=="m10_02_00_00"?"m40_02":"m40_03";
 Check(env.Textures.Count==(source=="m10_02_00_00"?10:12),"Shared area lost compatibility maps or earlier destination aliases");
 foreach(var original in oldEnv.Textures){string name=original.Name.Replace("m24_","m40_");Check(env.Textures.Any(t=>t.Name==name),"Full canonical area compatibility name was pruned");if(name.StartsWith("m40_00_",StringComparison.Ordinal)||name.StartsWith("m40_01_",StringComparison.Ordinal))Check(env.Textures.Single(t=>t.Name==name).Bytes.AsSpan().SequenceEqual(original.Bytes),"Unrelated canonical area environment changed");}
 if(previousEnvironment!=null)foreach(var prior in TPF.Read(previousEnvironment).Textures){var retained=env.Textures.Single(t=>t.Name==prior.Name);Check(retained.Bytes.AsSpan().SequenceEqual(prior.Bytes)&&retained.Format==prior.Format&&retained.Mipmaps==prior.Mipmaps&&retained.Type==prior.Type,"Secondary destination clobbered previous area/Firelink environment");}previousEnvironment=files["map/m40/m40_envmap.tpf.dcx"].ToArray();
 foreach(var original in oldEnv.Textures.Where(t=>t.Name.StartsWith(sourceBlock+"_GILM",StringComparison.Ordinal))){var mapped=env.Textures.Single(t=>t.Name==targetBlock+original.Name[6..]);Check(mapped.Bytes.AsSpan().SequenceEqual(original.Bytes)&&mapped.Format==original.Format&&mapped.Mipmaps==original.Mipmaps&&mapped.Type==original.Type,"Block environment pixels/layout changed");}
 Check(env.Textures.Where(t=>t.Name.StartsWith("m40_02_",StringComparison.Ordinal)).Select(t=>t.Name).SequenceEqual(new[]{"m40_02_GILM0000","m40_02_GILM0001","m40_02_GILM0002","m40_02_GILM0003"}),"Firelink reflection names no longer match the native block/EnvMapID contract");
 var seededEnv=files["map/m40/m40_envmap.tpf.dcx"];Check(MapDestination.EnvironmentMaps(choice,Read("map/m24/m24_envmap.tpf.dcx")!,seededEnv).AsSpan().SequenceEqual(seededEnv),"Repeated destination seed changed/duplicated environment aliases");
 var grading=TPF.Read(Read("map/m24/m24_cgrading.tpf.dcx")!);var copiedGrading=TPF.Read(files["map/m40/m40_cgrading.tpf.dcx"]);Check(grading.Textures.Count==copiedGrading.Textures.Count,"Color grading changed under environment fix");for(int i=0;i<grading.Textures.Count;i++)Check(copiedGrading.Textures[i].Name==grading.Textures[i].Name.Replace("m24_","m40_")&&copiedGrading.Textures[i].Bytes.AsSpan().SequenceEqual(grading.Textures[i].Bytes),"Color grading bytes/name policy changed");
 string talkPath="script/talk/"+target+".talkesdbnd.dcx";var talk=BND4.Read(files[talkPath]);var e=ESD.Read(talk.Files[0].Bytes);e.Name=target=="m40_02_00_00"?"t402970":"t403970";talk.Files.Add(new BinderFile(Binder.FileFlags.Flag1,talk.Files.Max(f=>f.ID)+1,e.Name+".esd",e.Write()));files[talkPath]=talk.Write(DCX.Type.DCX_DFLT_10000_44_9);
 var actor=new MSBB.Part.Enemy{Name="c8900_0000",ModelName="c8900",EntityID=target=="m40_02_00_00"?2419700:4039000,TalkID=int.Parse(e.Name[1..]),CollisionName=map.Parts.Collisions[0].Name};map.Models.Enemies.Add(new MSBB.Model.Enemy("c8900"));map.Parts.Enemies.Add(actor);
 var collision=map.Parts.Collisions.Single();Array.Fill(collision.DrawGroups,2u);Array.Fill(collision.DispGroups,4u);
 collision.DisableStart=true;collision.EntityID=4012345;collision.HitFilterID=8;collision.GparamConfigStruct.LightSetID=3;
 var originalCollision=(MSBB.Part.Collision)collision.DeepCopy();var actorBefore=JsonSerializer.Serialize(actor);
 MapDestination.Finish(map,choice,Read,Write);Check(!map.Parts.Objects.Any(p=>p.Name==MapDestination.UrnSeed)&&map.Parts.Objects.Count==0&&BND4.Read(files[talkPath]).Files.Count==1,"Template actor/talk leaked into additional scene");Check(before.SequenceEqual(donor.Write()),"Donor map mutated");
 Check(collision.DrawGroups.All(v=>v==uint.MaxValue),"Additional floors lack full H/L residency");
 Check(collision.DispGroups.SequenceEqual(originalCollision.DispGroups),"Residency exposed distant geometry by changing Display groups");
 var normalized=(MSBB.Part.Collision)collision.DeepCopy();Array.Copy(originalCollision.DrawGroups,normalized.DrawGroups,8);
 Check(JsonSerializer.Serialize(normalized)==JsonSerializer.Serialize(originalCollision),"Residency changed collision identity, enable flags, physical filter or lighting");
 Check(JsonSerializer.Serialize(actor)==actorBefore,"Residency changed actor groups or metadata");
 var tex=MapDestination.TextureArchive(choice,Read);Check(tex.Files.Count==(target=="m40_02_00_00"?0:1),"Texture composition lost earlier entries or copied native terrain");
 if(tex.Files.Count==1)Check(tex.Files[0].Name=="m40_import_first.tpf.dcx"&&tex.Files[0].Bytes.Span.SequenceEqual(new byte[]{1,2,3}),"Earlier imported texture was changed");
 tex.Files.Add(new BinderFile(Binder.FileFlags.Flag1,tex.Files.Count,target=="m40_02_00_00"?"m40_import_first.tpf.dcx":"m40_import_second.tpf.dcx",new byte[]{1,2,3}));tex.Write(out var th,out var td);files["map/m40/m40_0003.tpfbhd"]=th;files["map/m40/m40_0003.tpfbdt"]=td;
 MapDestination.VerifyNativeEntityReservations(root,map);
 if(target=="m40_03_00_00"){actor.EntityID=2409000;bool rejected=false;try{MapDestination.VerifyNativeEntityReservations(root,map);}catch(InvalidDataException){rejected=true;}Check(rejected,"Native Burg2409000 conflict escaped the global guard");actor.EntityID=4039000;}
 Console.WriteLine("Destination PASS "+target);
}
void Reject(Action action){try{action();}catch(NotSupportedException){return;}throw new Exception("Unsupported destination accepted");}
Reject(()=>MapDestination.Select("additional","DS2S","m10_04_00_00","m40_02_00_00"));Reject(()=>MapDestination.Select("additional","DS1R","m10_01_00_00","m40_02_00_00"));Reject(()=>MapDestination.Select("replacement","DS1R","m10_02_00_00","m40_02_00_00"));
var replacement=MapDestination.Select("replacement","DS1R","m10_02_00_00","m24_01_00_01");Check(!replacement.Additional&&replacement.TemplateMap=="m24_01_00_01","Replacement selection changed");
var originalMap=MSBB.Read(Read("map/MapStudio/m24_01_00_01.msb.dcx")!);var replacementBefore=originalMap.Write();int writes=files.Count;
MapDestination.Finish(originalMap,replacement,Read,Write);Check(originalMap.Write().AsSpan().SequenceEqual(replacementBefore)&&files.Count==writes,"Additional residency policy changed a replacement or native map");
byte[] prefix=[0xff,0x82,0xa0,13,10];var appended=MapDestination.AppendLoadList(prefix,"m40_02_00_00");Check(appended.AsSpan(0,prefix.Length).SequenceEqual(prefix)&&appended.SequenceEqual(MapDestination.AppendLoadList(appended,"m40_02_00_00")),"Area list changed original bytes or duplicated a map");
var loadlist=System.Text.Encoding.ASCII.GetString(files["map/m40/m40.arealoadlist"]);Check(loadlist.Contains("m40_02")&&loadlist.Contains("m40_03"),"Composed area list lost earlier map");foreach(var (p,h) in hashes)Check(h.SequenceEqual(SHA256.HashData(File.ReadAllBytes(Path.Combine(root,p)))),"Original mutated: "+p);Console.WriteLine("PASS source hashes "+hashes.Count);
void Invalid(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Conflicting environment aliases accepted");}
var firelink=MapDestination.Select("additional","DS1R","m10_02_00_00","m40_02_00_00");var donorEnv=Read("map/m24/m24_envmap.tpf.dcx")!;var existingEnv=files["map/m40/m40_envmap.tpf.dcx"];
var badEnv=TPF.Read(existingEnv);badEnv.Textures.Single(t=>t.Name=="m40_02_GILM0000").Bytes[0]^=1;Invalid(()=>MapDestination.EnvironmentMaps(firelink,donorEnv,badEnv.Write()));
badEnv=TPF.Read(existingEnv);badEnv.Textures.Single(t=>t.Name=="m40_02_GILM0000").Mipmaps++;Invalid(()=>MapDestination.EnvironmentMaps(firelink,donorEnv,badEnv.Write()));
badEnv=TPF.Read(existingEnv);badEnv.Textures.Add(TPF.Read(existingEnv).Textures[0]);Invalid(()=>MapDestination.EnvironmentMaps(firelink,donorEnv,badEnv.Write()));
var foreign=TPF.Read(existingEnv);var extra=TPF.Read(donorEnv).Textures[0];extra.Name="unrelated_owned_environment";foreign.Textures.Add(extra);var merged=TPF.Read(MapDestination.EnvironmentMaps(firelink,donorEnv,foreign.Write()));Check(merged.Textures.Last().Name==extra.Name&&merged.Textures.Last().Bytes.AsSpan().SequenceEqual(extra.Bytes),"Environment reseed discarded unrelated owned texture");
Invalid(()=>MapDestination.EnvironmentMaps(firelink,new TPF().Write(),null));
var thin=TPF.Read(existingEnv);thin.Textures.RemoveAll(t=>t.Name.StartsWith("m40_00_",StringComparison.Ordinal)||t.Name.StartsWith("m40_01_",StringComparison.Ordinal));var restored=TPF.Read(MapDestination.EnvironmentMaps(firelink,donorEnv,thin.Write()));Check(restored.Textures.Count==12&&restored.Textures.Any(t=>t.Name=="m40_00_GILM0000")&&restored.Textures.Any(t=>t.Name=="m40_01_GILM0003"),"Compatibility maps missing from an existing stage were not restored");
 Console.WriteLine("PASS full area compatibility retention, exact block aliases, sequential merge/idempotence, conflicting content rejection and unrelated texture preservation");
