if not IsBound(ORDER) then
  Error("usage: gap -q -c 'ORDER:=8;; OUTPUT:=\"out.txt\";; WITH_INDEX:=true;;' generate_tables.g");
fi;

n := ORDER;;
if not IsInt(n) or n < 1 then
  Error("ORDER must be a positive integer");
fi;

if IsBound(OUTPUT) then
  OutputName := OUTPUT;;
else
  OutputName := Concatenation("cayley_tables_order", String(n), ".txt");;
fi;

if IsBound(WITH_INDEX) then
  WithIndex := WITH_INDEX;;
else
  WithIndex := false;;
fi;

Alphabet := "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";;
if n > Length(Alphabet) then
  Error("order ", n, " is too large for the built-in one-character alphabet");
fi;

Groups := AllSmallGroups(n);;
Expected := Sum(Groups, G -> Factorial(n - 1) / Size(AutomorphismGroup(G)));;

Print("Order: ", n, "\n");
Print("Groups: ", Length(Groups), "\n");
Print("Expected distinct tables: ", Expected, "\n");
Print("Output: ", OutputName, "\n");
if WithIndex then
  Print("Row format: <group_index> <serialized_table>\n");
else
  Print("Row format: <serialized_table>\n");
fi;

out := OutputTextFile(OutputName, false);;
SetPrintFormattingStatus(out, false);;
total_written := 0;;

for group_idx in [1..Length(Groups)] do
  G := Groups[group_idx];;
  Els := Elements(G);;

  if Position(Els, Identity(G)) <> 1 then
    Error("identity is not first for group ", group_idx, ": ", IdGroup(G));
  fi;

  T := MultiplicationTable(G);;
  AutG := AutomorphismGroup(G);;
  aut_gens := GeneratorsOfGroup(AutG);;
  perm_gens := [];;

  for alpha in aut_gens do
    images := [];;
    for k in [1..n] do
      Add(images, Position(Els, Image(alpha, Els[k])));
    od;
    Add(perm_gens, PermList(images));
  od;

  S1 := Stabilizer(SymmetricGroup(n), 1);;
  AG := Group(perm_gens);;
  reps := RightTransversal(S1, AG);;
  expected_group := Factorial(n - 1) / Size(AG);;

  Print(
    "Group ", group_idx, "/", Length(Groups),
    " id=", IdGroup(G),
    " aut=", Size(AG),
    " tables=", expected_group,
    "\n"
  );

  count := 0;;
  iter := Iterator(reps);;
  while not IsDoneIterator(iter) do
    # RightTransversal gives representatives on the opposite side of the
    # relabeling action used below, so invert each representative.
    pi := Inverse(NextIterator(iter));;
    pi_inv := Inverse(pi);;
    s := "";;

    for i in [1..n] do
      for j in [1..n] do
        Add(s, Alphabet[T[i^pi][j^pi]^pi_inv]);
      od;
    od;

    if WithIndex then
      AppendTo(out, String(group_idx), " ", s, "\n");
    else
      AppendTo(out, s, "\n");
    fi;

    count := count + 1;;
    total_written := total_written + 1;;

    if count mod 1000000 = 0 then
      Print("  wrote ", count, "/", expected_group, " for this group; total=", total_written, "\n");
    fi;
  od;

  if count <> expected_group then
    Error("generated count mismatch for group ", group_idx);
  fi;
  Print("  finished group; total=", total_written, "\n");
od;

CloseStream(out);
Print("Finished. Total written: ", total_written, "\n");

if total_written <> Expected then
  Error("final count mismatch");
fi;
