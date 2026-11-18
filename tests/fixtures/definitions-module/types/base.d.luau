--!priority 10
declare extern type Vehicle with
	speed: number
	function drive(self, distance: number): number
end
