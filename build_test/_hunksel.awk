BEGIN { n=split(WANT,a," "); for(i=1;i<=n;i++) want[a[i]]=1; h=0; pre=1 }
/^@@/ { h++; pre=0; keep=(h in want)?1:0; if(keep) print; next }
pre==1 { print; next }
{ if(keep) print }
