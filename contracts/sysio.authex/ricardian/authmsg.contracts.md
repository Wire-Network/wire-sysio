<h1 class="contract">createlink</h1>

---
spec-version: "0.2.0"
title: Create Link.
summary: 'Records a one-to-one link between a Wire account and an external-chain public key.'
icon: https://wire.network/wp-content/uploads/2022/04/WIRE_Icon_White_min.png
---

<b>Description:</b>
<div class="description">
From a user's external wallet signature and their Wire-formatted public key, a link is recorded tying the external-chain identity to the Wire account. The signature is verified against the supplied public key before the link is stored. Creating a link records this mapping only; it grants no permission and no additional authority on the Wire account.
</div>

<b>Clauses:</b>
<div class="clauses">
Links are one to one: an account may hold a single link per external chain, and an external-chain public key may be linked to a single account.
</div>
