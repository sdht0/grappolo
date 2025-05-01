import sys

limit = int(sys.argv[2])

with open(f"{sys.argv[3]}/nodes.csv", "w") as out:
    for i in range(limit+1):
        out.write(f"{i}\n")

with open(sys.argv[1]) as f:
    with open(f"{sys.argv[3]}/edges.csv", "w") as out:
        for line in f:
            parts = line.strip().split(",")
            src = int(parts[0])
            dst = int(parts[1])
            if src <= limit and dst <= limit:
                out.write(line)
