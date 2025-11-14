<h1 class="contract">createlink</h1>

---
spec-version: "0.2.0"
title: Create Link.
summary: 'Creates account link for WNS ecosystem. Granting custom curve.ext permissions.'
icon: https://wire.network/wp-content/uploads/2022/04/WIRE_Icon_White_min.png
---

<b>Description:</b>
<div class="description">
From a users External Wallet Signature and Wire formatted Pub_Key a link is established to tying your external chain identity to your Wire account. Successful link creation will grant you a special permission assigned with your external chains Pub_Key allowing you to transact on Wire with external wallets.
</div>

<b>Clauses:</b>
<div class="clauses">
Links between external addresses and wire names are one to one.
</div>




<h1 class="contract">onlinkauth</h1>

---
spec-version: "0.2.0"
title: On Link Auth.
summary: 'Notifies sysio system contract to add special permission to the account.'
icon: https://bucket.gitgo.app/frontend-assets/icons/wire-brandmark-transparent.png
---

<b>Description:</b>
<div class="description">
Inline action called upon successfully verifying a user in the 'createlink' action. Notifies the sysio system contract of a successful link which prompts the core to add the custom permission of 'curve.ext' (Where curve is EC / ED depending on the chain) to the account, allowing users to access the WNS eco system.
</div>

<b>Clauses:</b>
<div class="clauses">
auth.ext is a special permission that allows access to the WNS eco system, which is only assignable by sysio core via 'createlink'.
</div>




<h1 class="contract">onmanualrmv</h1>

---
spec-version: "0.2.0"
title: On Manual Remove.
summary: 'Upon notification that auth.ext was removed from an account, updates the links table.'
icon: https://bucket.gitgo.app/frontend-assets/icons/wire-brandmark-transparent.png
---

<b>Description:</b>
<div class="description">
When a curve.ext permission is removed from an account, auth.msg is notified and the links table is updated accordingly. This is a safety precaution, users are not able to remove this permission manually by default.
</div>

<b>Clauses:</b>
<div class="clauses">
curve.ext are special permissions that allows access to the WNS eco system, which is only assignable by sysio via 'createlink'. Any time it is removed from an account the links table MUST reflect this.
</div>
